/**
 * @file probability_store.c
 * @brief Write-once probabilities and their rollback.
 *
 * The circuit store is append-only: a gate, once created, never
 * changes, and a gate created by a transaction that later rolls back
 * is an orphan rather than an inconsistency -- no committed row can
 * reference it, and the same expression recomputed lands on the same
 * content-addressed UUID.  A probability is the one thing about a gate
 * that used to be rewritten in place, which is what made a rolled-back
 * @c set_prob stick.
 *
 * This file makes a probability a fact appended to the circuit like
 * the gate itself:
 *
 *  - @c set_prob writes it when the gate has none, reports success
 *    without writing when the gate already holds exactly that value
 *    (so setup scripts and notebook cells stay re-runnable, the same
 *    idempotence @c create_gate and @c add_provenance offer), and
 *    raises otherwise;
 *  - every write is recorded in a backend-local list keyed by
 *    subtransaction nesting level, and an aborted (sub)transaction
 *    clears the probabilities it wrote.  No old value is kept, because
 *    there never was one: this is an undo list of tokens, not of
 *    values;
 *  - changing a probability means minting a fresh input gate with the
 *    new probability and rewriting the rows that carry the old token,
 *    which @c provsql.replace_input does; the base table's token
 *    column is the place of truth, exactly as it is for the token
 *    rewrites the data-modification triggers perform.
 *
 * What is not closed here is isolation: another session can read a
 * probability between the write and the rollback.  Closing that would
 * mean deferring the write to @c XACT_EVENT_PRE_COMMIT behind a
 * backend-local overlay, at the price of making every reader consult
 * the overlay.
 */
#include "postgres.h"

#include <math.h>

#include "access/xact.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/uuid.h"

#include "probability_store.h"
#include "provsql_mmap.h"
#include "provsql_utils.h"

/**
 * @brief One recorded probability write, awaiting commit or rollback.
 *
 * @c nest_level is the subtransaction nesting level the write happened
 * at (@c GetCurrentTransactionNestLevel).  A subtransaction abort
 * clears every entry at or below its own level; a subtransaction
 * commit re-parents them to the parent level, so an outer @c ROLLBACK
 * still undoes a write made inside a released savepoint.
 */
typedef struct prob_write {
  pg_uuid_t token;
  int       nest_level;
} prob_write;

static prob_write *prob_writes = NULL;
static int prob_writes_len = 0;
static int prob_writes_cap = 0;
static bool prob_callbacks_registered = false;

/** Replacement leaves minted by this transaction; see the block below. */
static pg_uuid_t *fresh_leaves = NULL;
static int fresh_leaves_len = 0;
static int fresh_leaves_cap = 0;

/** @brief Drop the recorded writes at nesting level @p level or deeper,
 *  clearing each one's probability in the store when @p undo. */
static void prob_writes_unwind(int level, bool undo)
{
  int keep = 0;

  for(int i = 0; i < prob_writes_len; ++i) {
    if(prob_writes[i].nest_level >= level) {
      if(undo)
        provsql_internal_clear_prob(&prob_writes[i].token);
    } else {
      prob_writes[keep++] = prob_writes[i];
    }
  }
  prob_writes_len = keep;
}

/** @brief Move the recorded writes at nesting level @p level up to its
 *  parent, so a later outer rollback still undoes them. */
static void prob_writes_reparent(int level)
{
  for(int i = 0; i < prob_writes_len; ++i)
    if(prob_writes[i].nest_level >= level)
      prob_writes[i].nest_level = level - 1;
}

/** @brief Release the list without touching the store. */
static void prob_writes_forget(void)
{
  prob_writes_len = 0;
}

static void prob_xact_callback(XactEvent event, void *arg)
{
  (void) arg;
  switch(event) {
  case XACT_EVENT_COMMIT:
  case XACT_EVENT_PARALLEL_COMMIT:
  case XACT_EVENT_PREPARE:
    prob_writes_forget();
    fresh_leaves_len = 0;
    break;
  case XACT_EVENT_ABORT:
  case XACT_EVENT_PARALLEL_ABORT:
    /* No SPI and no new transaction here, but a pipe write is fine: the
       worker is a separate process and the message needs no snapshot.
       An error raised here would escalate to FATAL -- the transaction is
       already aborting -- so a store that has become unreachable is
       reported and the list dropped, rather than taking the session down
       with it. */
    PG_TRY();
    {
      prob_writes_unwind(0, true);
    }
    PG_CATCH();
    {
      prob_writes_len = 0;
      FlushErrorState();
      provsql_warning("could not clear the probabilities written by the "
                      "transaction that just rolled back; they stand as "
                      "written");
    }
    PG_END_TRY();
    fresh_leaves_len = 0;
    break;
  case XACT_EVENT_PRE_PREPARE:
    if(prob_writes_len > 0)
      provsql_error("cannot PREPARE a transaction that has written "
                    "provenance probabilities: the write would have to be "
                    "undone if the prepared transaction rolled back, and "
                    "ProvSQL keeps that undo list in the backend");
    break;
  default:
    break;
  }
}

static void prob_subxact_callback(SubXactEvent event,
                                  SubTransactionId mySubid,
                                  SubTransactionId parentSubid,
                                  void *arg)
{
  (void) mySubid; (void) parentSubid; (void) arg;
  switch(event) {
  case SUBXACT_EVENT_ABORT_SUB:
    /* Same reasoning as the transaction-level abort above. */
    PG_TRY();
    {
      prob_writes_unwind(GetCurrentTransactionNestLevel(), true);
    }
    PG_CATCH();
    {
      prob_writes_unwind(GetCurrentTransactionNestLevel(), false);
      FlushErrorState();
      provsql_warning("could not clear the probabilities written by the "
                      "subtransaction that just rolled back; they stand "
                      "as written");
    }
    PG_END_TRY();
    break;
  case SUBXACT_EVENT_COMMIT_SUB:
    prob_writes_reparent(GetCurrentTransactionNestLevel());
    break;
  default:
    break;
  }
}

/** @brief Record that this transaction wrote @p token's probability. */
static void prob_writes_record(const pg_uuid_t *token)
{
  if(!prob_callbacks_registered) {
    RegisterXactCallback(prob_xact_callback, NULL);
    RegisterSubXactCallback(prob_subxact_callback, NULL);
    prob_callbacks_registered = true;
  }

  if(prob_writes_len == prob_writes_cap) {
    int newcap = prob_writes_cap ? prob_writes_cap * 2 : 64;
    prob_write *grown = realloc(prob_writes, newcap * sizeof(prob_write));
    if(!grown)
      provsql_error("ProvSQL: out of memory recording a probability write");
    prob_writes = grown;
    prob_writes_cap = newcap;
  }
  prob_writes[prob_writes_len].token = *token;
  prob_writes[prob_writes_len].nest_level = GetCurrentTransactionNestLevel();
  ++prob_writes_len;
}

/* -------------------------------------------------------------------------
 * Freshly minted replacement leaves
 *
 * @c provsql.replace_input and its siblings mint a new leaf gate for a row
 * whose probability is to change; the row's provsql column is then rewritten
 * to carry it.  @c provenance_guard cannot tell such a token from an
 * arbitrary UUID a user pasted in, and it flips a table to OPAQUE on the
 * latter because TID independence can no longer be assumed.  A replacement
 * leaf *is* an independent fresh leaf, so the table stays what it was --
 * provided the guard can recognise it, which is what this list is for.
 *
 * The list is per-transaction: a token minted by a transaction that rolls
 * back never reaches a committed row.
 * ------------------------------------------------------------------------- */

/** @brief Remember that this transaction minted @p token as a replacement
 *  leaf for a tracked row. */
static void fresh_leaves_record(const pg_uuid_t *token)
{
  if(fresh_leaves_len == fresh_leaves_cap) {
    int newcap = fresh_leaves_cap ? fresh_leaves_cap * 2 : 64;
    pg_uuid_t *grown = realloc(fresh_leaves, newcap * sizeof(pg_uuid_t));
    if(!grown)
      provsql_error("ProvSQL: out of memory recording a replacement leaf");
    fresh_leaves = grown;
    fresh_leaves_cap = newcap;
  }
  fresh_leaves[fresh_leaves_len++] = *token;
}

PG_FUNCTION_INFO_V1(note_fresh_leaf);
/**
 * @brief Declare a just-minted leaf gate a replacement for a tracked row.
 *
 * Called by @c provsql.replace_input / @c replace_block right after they
 * create the gate, so that the @c UPDATE which stores it does not look to
 * @c provenance_guard like a user pasting in an arbitrary token.
 */
Datum note_fresh_leaf(PG_FUNCTION_ARGS)
{
  if(PG_ARGISNULL(0))
    provsql_error("Invalid NULL value passed to note_fresh_leaf");
  fresh_leaves_record(DatumGetUUIDP(PG_GETARG_DATUM(0)));
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(is_fresh_leaf);
/** @brief Whether @p token was minted as a replacement leaf by this
 *  transaction (see @c note_fresh_leaf). */
Datum is_fresh_leaf(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token;

  if(PG_ARGISNULL(0))
    PG_RETURN_BOOL(false);

  token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  for(int i = 0; i < fresh_leaves_len; ++i)
    if(memcmp(&fresh_leaves[i], token, sizeof(pg_uuid_t)) == 0)
      PG_RETURN_BOOL(true);
  PG_RETURN_BOOL(false);
}

void provsql_set_prob_tracked(const pg_uuid_t *token, double prob)
{
  double existing = 0.;
  provsql_set_prob_result result;

  if(isnan(prob))
    provsql_error("set_prob: NaN is not a probability");
  if(prob < 0. || prob > 1.)
    provsql_error("set_prob: probability %g is outside [0, 1]", prob);

  result = provsql_internal_set_prob(token, prob, &existing);

  switch(result) {
  case PROVSQL_SET_PROB_WRITTEN:
    prob_writes_record(token);
    break;
  case PROVSQL_SET_PROB_UNCHANGED:
    break;
  case PROVSQL_SET_PROB_NOT_PROB_GATE:
    provsql_error("set_prob called on non-input gate");
    break;
  case PROVSQL_SET_PROB_ALREADY_SET:
  {
    unsigned nb_children = 0;
    pg_uuid_t *children = NULL;
    gate_type type = provsql_fetch_gate(token, &nb_children, &children);
    const char *hint;

    if(children)
      free(children);

    /* Which replacement applies depends on what the gate is: sending a
       block value or an update gate to replace_input() only earns the
       user a second refusal, since replace_input() redirects them here
       anyway. */
    switch(type) {
    case gate_mulinput:
      hint = "Use provsql.replace_block() to give a repair_key block a "
             "different set of probabilities: a block's values share one "
             "key gate and their masses are meaningful together, so they "
             "are replaced together.";
      break;
    case gate_update:
      hint = "Use provsql.replace_update() to give a recorded data "
             "modification a different probability.";
      break;
    default:
      hint = "Use provsql.replace_input() to give a tuple a different "
             "probability: it mints a fresh input gate and returns it, "
             "for the row's provsql column to carry.";
      break;
    }

    ereport(ERROR,
            (errmsg("probability of gate %s is already set to %g",
                    DatumGetCString(DirectFunctionCall1(
                                      uuid_out, UUIDPGetDatum(token))),
                    existing),
             errdetail("Probabilities are written once, so that a "
                       "transaction that rolls back leaves the circuit as "
                       "it found it."),
             errhint("%s", hint)));
    break;
  }
  }
}

PG_FUNCTION_INFO_V1(set_prob);
/**
 * @brief Write a gate's probability, once.
 *
 * Transparent @c gate_annotation wrappers (an inversion-free
 * certificate / order marker attached by the planner to a certified
 * query's row roots) are peeled first: a probability set on a wrapped
 * token belongs to the input gate underneath, so the documented
 * @c "set_prob(provenance(), p) FROM t" pattern keeps working when the
 * query happens to be certified.
 */
Datum set_prob(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token;
  double prob;
  pg_uuid_t peeled;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    provsql_error("Invalid NULL value passed to set_prob");

  token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  prob = PG_GETARG_FLOAT8(1);

  for(;;) {
    unsigned nb_children = 0;
    pg_uuid_t *children = NULL;
    gate_type type = provsql_fetch_gate(token, &nb_children, &children);
    if(type != gate_annotation || nb_children != 1) {
      if(children) free(children);
      break;
    }
    peeled = children[0];
    token = &peeled;
    free(children);
  }

  provsql_set_prob_tracked(token, prob);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(probability_is_set);
/**
 * @brief Report whether a probability has been written on a gate.
 *
 * @c get_prob() answers the value an evaluation would use, so it cannot
 * tell a gate nobody gave a probability from one written as 1.  This
 * can, which is what a UI needs in order to offer "set" on the first
 * and "replace" on the second.
 */
Datum probability_is_set(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  PG_RETURN_BOOL(provsql_internal_get_prob_written(token, NULL));
}

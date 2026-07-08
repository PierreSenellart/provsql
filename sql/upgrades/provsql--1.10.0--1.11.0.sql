-- ----------------------------------------------------------------------
-- provsql 1.10.0 -> 1.11.0
--
-- New SQL surface since 1.10.0:
--   * Maintained provenance mappings.  create_provenance_mapping(...,
--     maintained => true) registers the mapping in the new
--     provenance_mapping_registry; provenance_guard then appends each
--     genuine insert to it (keyed to the freshly minted input token), so
--     the mapping stays current AND survives the provsql rewrites that data
--     modification performs.  This fixes the temporal validity reported for
--     a row after it is deleted/updated: a view-based mapping keyed validity
--     on the live (rewritten) provsql column and so lost the row's original
--     interval, while a maintained mapping keeps it keyed to the original
--     input token (the child a later monus/update gate wraps).
--     cleanup_table_info forgets a mapping when either table is dropped.
--   * create_provenance_mapping_view is removed (superseded by maintained
--     mapping tables).  The base time_validity_view, a plain view over the
--     append-only update_provenance log, is unchanged by this upgrade.
--   * The continuous-distribution surface: the Gamma / Chi-squared /
--     Log-normal / Weibull / Pareto / Beta families, the discrete counts
--     (poisson / binomial / geometric / hypergeometric / negative_binomial
--     over categorical_from_log_pmf), rv_families(), the gmm and
--     empirical_samples / empirical_cdf constructors.
--   * Function application on random variables: ^ / pow / power / ln /
--     exp / sqrt, and the RV-typed CASE lowering targets provenance_case /
--     rv_case with the random_variable btree operator class that lets
--     GREATEST / LEAST parse (greatest / least order statistics, min / max
--     / rv_sum_or_null aggregates with per-aggregate identities).
--   * The aggregate-carrier CASE: agg_case, agg_gate_value /
--     agg_guard_holds (actual-world display values), agg_defined_event and
--     the conditional-on-defined agg_raw_moment (incl. the AVG arm over
--     agg_avg_moment_exact), and the extended agg_token_value_text.
--   * SQL-standard statistic aggregates over random_variable rows:
--     covar_pop / covar_samp / corr / stddev_pop / stddev_samp and the
--     ordered-set percentile_cont, with their rv_*_impl rewrite targets,
--     rv_stat_* circuit builders, and the rv_percentile_state transition
--     type.
--   * Readouts: probability (uuid alias + boolean-predicate overload with
--     the (A) | (B) conditioning operator), quantile / rv_quantile,
--     covariance / correlation / stddev, entropy / kl /
--     mutual_information with their rv_* bindings.
-- ----------------------------------------------------------------------

SET search_path TO provsql;

-- Guarded-selection gate for the RV / aggregate CASE lowering (rv_case /
-- agg_case below).  ADD VALUE is upgrade-safe here: the value is only
-- referenced as a literal inside function bodies created by this script,
-- never materialised in this transaction (same pattern as 1.9.0 -> 1.10.0's
-- 'conditioned').
ALTER TYPE provenance_gate ADD VALUE IF NOT EXISTS 'case';
-- 'observe' gate: the likelihood-weighting observation leaf built by
-- observe(random_variable, datum).  Only referenced at runtime (as the
-- create_gate type text), never in this script, so ADD VALUE is
-- transaction-safe on the PG12+ upgrade path exactly like 'case' above.
ALTER TYPE provenance_gate ADD VALUE IF NOT EXISTS 'observe';

-- Registry backing maintained mappings.
CREATE TABLE IF NOT EXISTS provsql.provenance_mapping_registry(
  mapping   oid PRIMARY KEY,
  source    oid NOT NULL,
  attribute name NOT NULL
);
CREATE INDEX IF NOT EXISTS provenance_mapping_registry_source_idx
  ON provsql.provenance_mapping_registry(source);

-- create_provenance_mapping gains the `maintained` argument: a signature
-- change, so drop the old four-argument form before recreating.
DROP FUNCTION IF EXISTS create_provenance_mapping(text, regclass, text, bool);
CREATE OR REPLACE FUNCTION create_provenance_mapping(
  newtbl text,
  oldtbl regclass,
  att text,
  preserve_case bool DEFAULT 'f',
  maintained bool DEFAULT false
) RETURNS void AS
$$
DECLARE
BEGIN
  -- Idempotence: when the mapping table already exists, leave it alone
  -- with a NOTICE (re-runnable setup scripts / notebook cells). Drop it
  -- first to rebuild a stale mapping.
  IF (CASE WHEN preserve_case THEN to_regclass(format('%I', newtbl))
           ELSE to_regclass(newtbl) END) IS NOT NULL THEN
    RAISE NOTICE 'mapping table % already exists', newtbl;
    RETURN;
  END IF;
  -- ON COMMIT DROP only fires at COMMIT: several mapping creations in
  -- one transaction (a notebook cell, a setup script run via psql -1)
  -- would otherwise collide on the leftover temp table. The to_regclass
  -- probe (rather than DROP IF EXISTS) keeps the first call NOTICE-free.
  IF to_regclass('pg_temp.tmp_provsql') IS NOT NULL THEN
    DROP TABLE tmp_provsql;
  END IF;
  EXECUTE format('CREATE TEMP TABLE tmp_provsql ON COMMIT DROP AS TABLE %s', oldtbl);
  ALTER TABLE tmp_provsql RENAME provsql TO provenance;
  IF preserve_case THEN
    EXECUTE format('CREATE TABLE %I AS SELECT %s AS value, provenance FROM tmp_provsql', newtbl, att);
    EXECUTE format('CREATE INDEX ON %I(provenance)', newtbl);
  ELSE
    EXECUTE format('CREATE TABLE %s AS SELECT %s AS value, provenance FROM tmp_provsql', newtbl, att);
    EXECUTE format('CREATE INDEX ON %s(provenance)', newtbl);
  END IF;
  IF maintained THEN
    -- Register so genuine inserts into oldtbl keep the mapping current
    -- (see provenance_guard); keyed to the input token, so it survives the
    -- provsql rewrites that data modification performs.
    INSERT INTO provsql.provenance_mapping_registry(mapping, source, attribute)
      VALUES (
        (CASE WHEN preserve_case THEN to_regclass(format('%I', newtbl))
              ELSE to_regclass(newtbl) END)::oid,
        oldtbl::oid, att)
      ON CONFLICT (mapping)
        DO UPDATE SET source = EXCLUDED.source, attribute = EXCLUDED.attribute;
  END IF;
END
$$ LANGUAGE plpgsql;

-- provenance_guard appends to registered mappings on genuine inserts.
CREATE OR REPLACE FUNCTION provenance_guard()
  RETURNS TRIGGER AS $$
DECLARE
  _m RECORD;
BEGIN
  IF TG_OP = 'INSERT' THEN
    IF NEW.provsql IS NULL THEN
      -- A genuine insert: mint a fresh atomic input variable. This is the
      -- one place a new input token is born, so it is also where any
      -- maintained mapping on this table is extended (keyed to that token).
      -- Data-modification re-insertions (INSERT ... SELECT * FROM OLD_TABLE)
      -- carry a supplied provsql and take the ELSE branch, so they are
      -- correctly skipped: the validity stays keyed to the original input,
      -- which is exactly the child a later monus/update gate wraps.
      NEW.provsql := public.uuid_generate_v4();
      FOR _m IN SELECT mapping, attribute
                  FROM provsql.provenance_mapping_registry WHERE source = TG_RELID
      LOOP
        EXECUTE format(
          'INSERT INTO %s(value, provenance) SELECT ($1).%I, $2',
          _m.mapping::regclass, _m.attribute)
          USING NEW, NEW.provsql;
      END LOOP;
    ELSE
      PERFORM provsql.set_table_info(TG_RELID, 'opaque');
    END IF;
  ELSIF TG_OP = 'UPDATE' THEN
    IF NEW.provsql IS DISTINCT FROM OLD.provsql THEN
      PERFORM provsql.set_table_info(TG_RELID, 'opaque');
    END IF;
  END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER;

-- cleanup_table_info also forgets registry entries when a table is dropped.
CREATE OR REPLACE FUNCTION cleanup_table_info()
  RETURNS event_trigger AS
$$
DECLARE
  r RECORD;
BEGIN
  FOR r IN
    SELECT objid FROM pg_event_trigger_dropped_objects()
     WHERE object_type IN ('table', 'foreign table', 'materialized view')
  LOOP
    PERFORM provsql.remove_table_info(r.objid);
    -- Forget any maintained mapping whose source or mapping table is gone.
    DELETE FROM provsql.provenance_mapping_registry
     WHERE source = r.objid OR mapping = r.objid;
  END LOOP;
END
$$ LANGUAGE plpgsql;

-- The view-based mapping helper is removed (superseded by maintained tables).
DROP FUNCTION IF EXISTS create_provenance_mapping_view(text, regclass, text, bool);

/**
 * @brief Placeholder for @c "(predicate) | (predicate)" on two events.
 *
 * Conditions one comparison event on another when both operands are written
 * as comparisons rather than pre-built tokens (e.g.
 * @c "probability((x >= 2000) | (x >= 1000))"): an @c random_variable /
 * @c agg_token comparison is statically @c boolean-typed, so neither the
 * @c "uuid | uuid" (@c cond) nor the @c "uuid | boolean" (@c cond_predicate)
 * operator resolves.  Never executes: the ProvSQL planner hook lowers each
 * Boolean operand to its event gate and emits @c cond(target, evidence), so
 * the result carries the correlation-aware @c Pr(A ∧ B) / Pr(B).  Returns
 * @c uuid, so @c "A | B" is a first-class event token in every position
 * (a @c probability(uuid) argument, a projected column, a further @c "|").
 */
CREATE OR REPLACE FUNCTION predicate_cond_predicate(target boolean, evidence boolean)
  RETURNS UUID AS
$$
BEGIN
  RAISE EXCEPTION '(predicate) | (predicate) must be rewritten by the ProvSQL '
    'planner hook: both operands must be Boolean combinations of '
    'random_variable / aggregate comparisons (is provsql.active off?)';
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o
      JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '|'
      AND o.oprleft = 'pg_catalog.bool'::regtype::oid
      AND o.oprright = 'pg_catalog.bool'::regtype::oid
      AND o.oprcode <> 0
  ) THEN
    CREATE OPERATOR | (LEFTARG=boolean, RIGHTARG=boolean, PROCEDURE=predicate_cond_predicate);
  END IF;
END $$;

/**
 * @brief Create a guarded-selection gate over scalar (RV) children.
 *
 * Builds a deterministic @c gate_case from the flattened wire list
 * @c [guard_1, value_1, ..., guard_k, value_k, default] (odd length): the
 * value of the first guard event that holds, else the default (first-match
 * semantics).  Each guard is a Boolean event token (a @c gate_cmp or Boolean
 * combination); each value and the default are scalar-producing gates
 * (@c gate_rv, @c gate_value, @c gate_arith, another @c gate_case, ...).  The
 * token UUID is derived deterministically from @p children so identical
 * @c CASE expressions share their gate.
 *
 * @param children  Flattened guard/value wires ending with the default
 *                  (@c array_length must be odd and @c >= 1).
 * @return  UUID of the (possibly pre-existing) @c gate_case.
 */
CREATE OR REPLACE FUNCTION provenance_case(
  children UUID[]
)
RETURNS UUID AS
$$
DECLARE
  case_token UUID;
BEGIN
  IF array_length(children, 1) IS NULL OR array_length(children, 1) % 2 = 0 THEN
    RAISE EXCEPTION 'provenance_case expects an odd number of children '
      '(guard/value pairs followed by a default), got %',
      coalesce(array_length(children, 1), 0);
  END IF;
  case_token := public.uuid_generate_v5(
    uuid_ns_provsql(),
    concat('case', children::text)
  );
  PERFORM create_gate(case_token, 'case', children);
  RETURN case_token;
END
$$ LANGUAGE plpgsql
  SET search_path=provsql,pg_temp,public
  SECURITY DEFINER
  IMMUTABLE
  PARALLEL SAFE
  STRICT;

/**
 * @brief Deterministic truth of a Boolean guard sub-circuit over aggregate
 *        comparisons, evaluated in the actual world (all input tuples present).
 *
 * Guards are the shapes @c having_Expr_to_provenance_cmp mints: @c cmp gates
 * over aggregate-valued children (comparison-operator OID in @c info1),
 * @c times / @c plus combinations (AND / OR, with negation pushed into the
 * comparison operators), and the @c one / @c zero indicators of regular
 * (aggregate-free) conditions.  Uses Kleene three-valued logic: returns
 * @c NULL on any other gate shape, or when an operand's deterministic value
 * cannot be resolved.
 */
CREATE OR REPLACE FUNCTION agg_guard_holds(token UUID)
  RETURNS boolean AS
$$
DECLARE
  gt provenance_gate := get_gate_type(token);
  ch uuid[];
  opname text;
  l numeric;
  r numeric;
  all_true boolean;
  any_true boolean;
  any_null boolean;
BEGIN
  IF gt = 'one' THEN
    RETURN true;
  ELSIF gt = 'zero' THEN
    RETURN false;
  ELSIF gt IN ('times', 'plus') THEN
    SELECT bool_and(h), bool_or(h), bool_or(h IS NULL)
      INTO all_true, any_true, any_null
      FROM (SELECT provsql.agg_guard_holds(c) AS h
            FROM unnest(get_children(token)) AS c) AS s;
    IF gt = 'times' THEN
      -- AND: false dominates unknown (bool_and skips NULL inputs, so it is
      -- false exactly when some child is false).
      RETURN CASE WHEN NOT all_true THEN false
                  WHEN any_null THEN NULL
                  ELSE true END;
    ELSE
      -- OR: true dominates unknown.
      RETURN CASE WHEN any_true THEN true
                  WHEN any_null THEN NULL
                  ELSE false END;
    END IF;
  ELSIF gt = 'cmp' THEN
    ch := get_children(token);
    l := agg_gate_value(ch[1]);
    r := agg_gate_value(ch[2]);
    IF l IS NULL OR r IS NULL THEN
      RETURN NULL;
    END IF;
    SELECT oprname INTO opname
      FROM pg_catalog.pg_operator WHERE oid = (get_infos(token)).info1;
    RETURN CASE opname
      WHEN '<'  THEN l <  r
      WHEN '<=' THEN l <= r
      WHEN '='  THEN l =  r
      WHEN '<>' THEN l <> r
      WHEN '>=' THEN l >= r
      WHEN '>'  THEN l >  r
    END;
  END IF;
  RETURN NULL;
END
$$ LANGUAGE plpgsql STABLE STRICT PARALLEL SAFE
  SET search_path=provsql,pg_temp,public;

/**
 * @brief Deterministic (actual-world) scalar value of an aggregate-carrying
 *        gate.
 *
 * Resolves the value an aggregate expression takes on the actual data -- the
 * value an @c agg_token display cell carries: @c agg / @c arith gates record
 * it in @c extra (set by aggregate evaluation and @c agg_arith_make), a
 * @c value gate carries its constant, a @c semimod wraps a value gate, a
 * @c conditioned gate has its target's value, and a @c case gate selects the
 * first branch whose guard holds in the actual world (per
 * @c agg_guard_holds), else the default.  Returns @c NULL when the gate is
 * not aggregate-carrying or the value cannot be resolved (e.g. a
 * non-numeric aggregate).
 */
CREATE OR REPLACE FUNCTION agg_gate_value(token UUID)
  RETURNS numeric AS
$$
DECLARE
  gt provenance_gate := get_gate_type(token);
  ch uuid[];
  n integer;
  holds boolean;
BEGIN
  IF gt IN ('agg', 'arith', 'value') THEN
    BEGIN
      RETURN get_extra(token)::numeric;
    EXCEPTION WHEN others THEN
      RETURN NULL;   -- non-numeric aggregate (e.g. min over text)
    END;
  ELSIF gt = 'semimod' THEN
    RETURN agg_gate_value((get_children(token))[2]);
  ELSIF gt = 'conditioned' THEN
    RETURN agg_gate_value((get_children(token))[1]);
  ELSIF gt = 'case' THEN
    ch := get_children(token);
    n := array_length(ch, 1);
    FOR i IN 1 .. (n - 1) / 2 LOOP
      holds := agg_guard_holds(ch[2 * i - 1]);
      IF holds IS NULL THEN
        RETURN NULL;
      ELSIF holds THEN
        RETURN agg_gate_value(ch[2 * i]);
      END IF;
    END LOOP;
    RETURN agg_gate_value(ch[n]);
  END IF;
  RETURN NULL;
END
$$ LANGUAGE plpgsql STABLE STRICT PARALLEL SAFE
  SET search_path=provsql,pg_temp,public;

/**
 * @brief Recover the @c "value (*)" display string for an aggregation gate
 *
 * Companion helper to the @c provsql.aggtoken_text_as_uuid GUC. With
 * the GUC on, an @c agg_token cell prints as the underlying provenance
 * UUID, which is convenient for tooling that wants to click through to
 * the circuit but loses the human-readable aggregate value. This
 * function takes such a UUID and returns the original @c "value (*)"
 * string by reading the gate's @c extra (set by aggregate evaluation
 * for @c agg gates, and by @c agg_arith_make for the @c arith gates
 * that agg_token arithmetic mints); for the other aggregate-carrying
 * gates (@c case, @c conditioned, @c semimod, @c value) the value is
 * resolved through the circuit by @c agg_gate_value. Returns @c NULL
 * if @p token does not resolve to an aggregate-carrying gate.
 *
 * @param token UUID of an @c agg gate (typically obtained from an
 *              @c agg_token cell when @c aggtoken_text_as_uuid is on,
 *              or via a manual UUID cast otherwise).
 */
CREATE OR REPLACE FUNCTION agg_token_value_text(token UUID)
  RETURNS text AS
$$
  SELECT CASE
    -- agg gates: extra is set by aggregate evaluation; arith gates
    -- (agg_token arithmetic): extra is recorded by agg_arith_make.
    WHEN provsql.get_gate_type(token) IN ('agg', 'arith')
      THEN provsql.get_extra(token) || ' (*)'
    -- other aggregate-carrying gates: resolve the actual-world value
    -- through the circuit.
    WHEN provsql.get_gate_type(token) IN ('case', 'conditioned', 'semimod', 'value')
      THEN provsql.agg_gate_value(token)::text || ' (*)'
    ELSE NULL
  END;
$$ LANGUAGE sql STABLE STRICT PARALLEL SAFE;

/**
 * @brief Construct a gamma-distribution random variable with shape @p k
 *        (any positive real) and rate @p lambda
 *
 * The gamma distribution generalises Erlang to non-integer shape; its
 * CDF is the regularised lower incomplete gamma, evaluated in closed
 * form by the analytic passes.  Sums of independent gammas with the
 * same rate fold to a single gamma in the simplifier.
 *
 * Validation:
 * - @p k must be finite and strictly positive.  An integer @p k (in
 *   @c integer range) is silently routed through @c erlang -- the gamma
 *   with integer shape *is* Erlang -- so <tt>gamma(2, λ)</tt> shares
 *   its gate encoding and closure interplay with <tt>erlang(2, λ)</tt>.
 * - @p lambda must be finite and strictly positive.
 *
 * @warning <tt>VOLATILE</tt> is load-bearing; see the warning on
 * @ref normal.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Gamma_distribution">Wikipedia: Gamma distribution</a>
 */
CREATE OR REPLACE FUNCTION gamma(k double precision, lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(k) THEN
    RAISE EXCEPTION 'provsql.gamma: k must be finite (got %)', k;
  END IF;
  IF k <= 0 THEN
    RAISE EXCEPTION 'provsql.gamma: k must be strictly positive (got %)', k;
  END IF;
  IF NOT provsql.is_finite_float8(lambda) THEN
    RAISE EXCEPTION 'provsql.gamma: lambda must be finite (got %)', lambda;
  END IF;
  IF lambda <= 0 THEN
    RAISE EXCEPTION 'provsql.gamma: lambda must be strictly positive (got %)', lambda;
  END IF;
  IF k = floor(k) AND k <= 2147483647 THEN
    RETURN provsql.erlang(k::integer, lambda);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'gamma:' || k || ',' || lambda);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct a chi-squared random variable with @p k degrees of
 *        freedom: syntactic sugar for <tt>gamma(k/2, 1/2)</tt>
 *
 * @p k is accepted as @c double @c precision so fractional degrees of
 * freedom work; it must be finite and strictly positive.  Even degrees
 * of freedom route through @c erlang via @c gamma's integer-shape rule.
 *
 * @warning <tt>VOLATILE</tt> is load-bearing; see the warning on
 * @ref normal.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Chi-squared_distribution">Wikipedia: Chi-squared distribution</a>
 */
CREATE OR REPLACE FUNCTION chi_squared(k double precision)
  RETURNS random_variable AS
$$
BEGIN
  IF NOT provsql.is_finite_float8(k) THEN
    RAISE EXCEPTION 'provsql.chi_squared: k must be finite (got %)', k;
  END IF;
  IF k <= 0 THEN
    RAISE EXCEPTION 'provsql.chi_squared: k must be strictly positive (got %)', k;
  END IF;
  RETURN provsql.gamma(k / 2, 0.5);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct a log-normal random variable: @c exp of a
 *        Normal(@p mu, @p sigma), parameterised by the underlying
 *        normal (so its median is <tt>exp(mu)</tt> and its mean
 *        <tt>exp(mu + sigma^2/2)</tt>)
 *
 * The multiplicative counterpart of @c normal: products of independent
 * lognormals fold to a lognormal in the simplifier, and the
 * <tt>exp(normal(...))</tt> / <tt>ln(lognormal(...))</tt> bridges fold
 * in both directions, so log-scale models stay closed-form.
 *
 * Validation mirrors @c normal: both parameters must be finite,
 * @p sigma non-negative; the degenerate @c sigma = 0 case is silently
 * routed through @c as_random (a Dirac at <tt>exp(mu)</tt>).
 *
 * @warning <tt>VOLATILE</tt> is load-bearing; see the warning on
 * @ref normal.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Log-normal_distribution">Wikipedia: Log-normal distribution</a>
 */
CREATE OR REPLACE FUNCTION lognormal(mu double precision, sigma double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(mu) OR NOT provsql.is_finite_float8(sigma) THEN
    RAISE EXCEPTION 'provsql.lognormal: parameters must be finite (got mu=%, sigma=%)', mu, sigma;
  END IF;
  IF sigma < 0 THEN
    RAISE EXCEPTION 'provsql.lognormal: sigma must be non-negative (got %)', sigma;
  END IF;
  IF sigma = 0 THEN
    RETURN provsql.as_random(exp(mu));
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'lognormal:' || mu || ',' || sigma);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct a Weibull random variable with shape @p k and
 *        scale @p lambda
 *
 * @p lambda is the SCALE (the 63.2% quantile), not a rate: @c k = 1 is
 * the exponential with rate <tt>1/lambda</tt>, and that case is
 * silently routed through @c exponential to share its gate.  The shape
 * tunes the hazard: @c k < 1 infant mortality, @c k > 1 wear-out.
 * Quantiles are exact, truncated moments are closed-form (via the
 * regularised incomplete gamma), and the min of i.i.d. Weibulls has a
 * closed-form mean (min-stability).
 *
 * Validation: both parameters must be finite and strictly positive.
 *
 * @warning <tt>VOLATILE</tt> is load-bearing; see the warning on
 * @ref normal.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Weibull_distribution">Wikipedia: Weibull distribution</a>
 */
CREATE OR REPLACE FUNCTION weibull(k double precision, lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(k) OR NOT provsql.is_finite_float8(lambda) THEN
    RAISE EXCEPTION 'provsql.weibull: parameters must be finite (got k=%, lambda=%)', k, lambda;
  END IF;
  IF k <= 0 OR lambda <= 0 THEN
    RAISE EXCEPTION 'provsql.weibull: parameters must be strictly positive (got k=%, lambda=%)', k, lambda;
  END IF;
  IF k = 1 THEN
    RETURN provsql.exponential(1 / lambda);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'weibull:' || k || ',' || lambda);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct a Pareto random variable with scale (minimum)
 *        @p xm and shape @p alpha
 *
 * The canonical heavy-tailed power law.  Raw moments are @b infinite
 * for <tt>alpha <= k</tt> and reported as <tt>Infinity</tt> (the mean
 * for <tt>alpha <= 1</tt>, the variance for <tt>alpha <= 2</tt>)
 * rather than estimated; quantiles, truncated moments, conditional
 * sampling (self-similarity: <tt>X | X > a</tt> is Pareto(a, alpha)),
 * and Pareto-vs-Pareto comparisons are all exact.
 *
 * Validation: both parameters must be finite and strictly positive.
 *
 * @warning <tt>VOLATILE</tt> is load-bearing; see the warning on
 * @ref normal.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Pareto_distribution">Wikipedia: Pareto distribution</a>
 */
CREATE OR REPLACE FUNCTION pareto(xm double precision, alpha double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(xm) OR NOT provsql.is_finite_float8(alpha) THEN
    RAISE EXCEPTION 'provsql.pareto: parameters must be finite (got xm=%, alpha=%)', xm, alpha;
  END IF;
  IF xm <= 0 OR alpha <= 0 THEN
    RAISE EXCEPTION 'provsql.pareto: parameters must be strictly positive (got xm=%, alpha=%)', xm, alpha;
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'pareto:' || xm || ',' || alpha);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION inverse_gamma(alpha double precision, beta double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(alpha) OR NOT provsql.is_finite_float8(beta) THEN
    RAISE EXCEPTION 'provsql.inverse_gamma: parameters must be finite (got alpha=%, beta=%)', alpha, beta;
  END IF;
  IF alpha <= 0 OR beta <= 0 THEN
    RAISE EXCEPTION 'provsql.inverse_gamma: parameters must be strictly positive (got alpha=%, beta=%)', alpha, beta;
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'inverse_gamma:' || alpha || ',' || beta);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION inverse_gaussian(mu double precision, lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(mu) OR NOT provsql.is_finite_float8(lambda) THEN
    RAISE EXCEPTION 'provsql.inverse_gaussian: parameters must be finite (got mu=%, lambda=%)', mu, lambda;
  END IF;
  IF mu <= 0 OR lambda <= 0 THEN
    RAISE EXCEPTION 'provsql.inverse_gaussian: parameters must be strictly positive (got mu=%, lambda=%)', mu, lambda;
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'inverse_gaussian:' || mu || ',' || lambda);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION wald(mu double precision, lambda double precision)
  RETURNS random_variable AS
$$
  SELECT provsql.inverse_gaussian(mu, lambda);
$$ LANGUAGE sql VOLATILE PARALLEL SAFE;

/**
 * @brief Build a discrete (categorical) random variable from outcomes
 *        and UNNORMALISED log-masses
 *
 * The shared back end of the discrete count constructors (@c poisson,
 * @c binomial, @c geometric, @c hypergeometric,
 * @c negative_binomial), and directly usable for any custom discrete
 * pmf: the log-masses are shifted by their maximum (so only relative
 * magnitudes matter and no @c exp underflows), outcomes whose relative
 * mass is below <tt>1e-15</tt> are dropped, and the rest is
 * renormalised before being handed to @c categorical.  Working in log
 * space keeps arbitrarily large parameters stable (e.g. a
 * <tt>Poisson(1000)</tt> pmf whose linear-space recurrence would
 * underflow at @c exp(-1000)).
 *
 * @param outcomes  outcome values, same length as @p log_pmf
 * @param log_pmf   natural logs of the (unnormalised) masses
 */
CREATE OR REPLACE FUNCTION categorical_from_log_pmf(
  outcomes double precision[], log_pmf double precision[])
  RETURNS random_variable AS
$$
DECLARE
  n int := array_length(outcomes, 1);
  max_lp double precision := '-Infinity';
  kept_o double precision[] := '{}';
  kept_p double precision[] := '{}';
  total double precision := 0;
  v double precision;
  i int;
BEGIN
  IF n IS NULL OR n = 0 OR n <> coalesce(array_length(log_pmf, 1), 0) THEN
    RAISE EXCEPTION 'provsql.categorical_from_log_pmf: outcomes and log_pmf must be non-empty arrays of the same length';
  END IF;
  FOR i IN 1..n LOOP
    IF log_pmf[i] > max_lp THEN max_lp := log_pmf[i]; END IF;
  END LOOP;
  IF max_lp = '-Infinity' THEN
    RAISE EXCEPTION 'provsql.categorical_from_log_pmf: all masses are zero';
  END IF;
  FOR i IN 1..n LOOP
    v := exp(log_pmf[i] - max_lp);
    IF v >= 1e-15 THEN
      kept_o := array_append(kept_o, outcomes[i]);
      kept_p := array_append(kept_p, v);
      total := total + v;
    END IF;
  END LOOP;
  FOR i IN 1..array_length(kept_p, 1) LOOP
    kept_p[i] := kept_p[i] / total;
  END LOOP;
  RETURN provsql.categorical(kept_p, kept_o);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct a Poisson random variable with mean @p lambda, as a
 *        truncated categorical
 *
 * The pmf is enumerated over <tt>[max(0, λ-12√λ), λ+12√λ+30]</tt> (the
 * omitted tails carry ~1e-30 of mass) by the log-space recurrence
 * <tt>ln p(k+1) = ln p(k) + ln λ - ln(k+1)</tt> and handed to
 * @c categorical_from_log_pmf, so moments, quantiles, and (in)equality
 * comparisons are exact over the enumerated support.  @c lambda = 0 is
 * a Dirac at @c 0 (routed through @c as_random); supports up to 10000
 * outcomes (λ up to ~170000), beyond which it raises -- approximate
 * huge means by @c normal(λ, √λ) instead.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Poisson_distribution">Wikipedia: Poisson distribution</a>
 */
CREATE OR REPLACE FUNCTION poisson(lambda double precision)
  RETURNS random_variable AS
$$
DECLARE
  lo int;
  hi int;
  outcomes double precision[] := '{}';
  lps double precision[] := '{}';
  lp double precision := 0;
  k int;
BEGIN
  IF NOT provsql.is_finite_float8(lambda) OR lambda < 0 THEN
    RAISE EXCEPTION 'provsql.poisson: lambda must be finite and non-negative (got %)', lambda;
  END IF;
  IF lambda = 0 THEN
    RETURN provsql.as_random(0);
  END IF;
  lo := greatest(0, floor(lambda - 12 * sqrt(lambda)))::int;
  hi := ceil(lambda + 12 * sqrt(lambda))::int + 30;
  IF hi - lo + 1 > 10000 THEN
    RAISE EXCEPTION 'provsql.poisson: support window of % outcomes exceeds 10000; approximate with normal(%, sqrt(%))', hi - lo + 1, lambda, lambda;
  END IF;
  -- ln p(0) = -λ; walk the recurrence, keeping only the window.
  lp := -lambda;
  FOR k IN 1..hi LOOP
    lp := lp + ln(lambda) - ln(k::double precision);
    IF k >= lo THEN
      outcomes := array_append(outcomes, k::double precision);
      lps := array_append(lps, lp);
    END IF;
  END LOOP;
  IF lo = 0 THEN
    outcomes := array_prepend(0::double precision, outcomes);
    lps := array_prepend(-lambda, lps);
  END IF;
  RETURN provsql.categorical_from_log_pmf(outcomes, lps);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct a Beta(α, β) random variable on the unit interval
 *
 * The conjugate prior of Bernoulli / binomial success probabilities:
 * closed-form moments, CDF through the regularised incomplete beta,
 * quantiles through the generic CDF bisection over the finite
 * @c [0, 1] support, and closed-form truncated moments (interval
 * conditioning).  <tt>Beta(1, 1)</tt> IS <tt>Uniform(0, 1)</tt> and is
 * silently routed through @c uniform to share its richer closed forms.
 *
 * Validation: both shapes must be finite and strictly positive.
 *
 * @warning <tt>VOLATILE</tt> is load-bearing; see the warning on
 * @ref normal.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Beta_distribution">Wikipedia: Beta distribution</a>
 */
CREATE OR REPLACE FUNCTION beta(alpha double precision, beta double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(alpha) OR NOT provsql.is_finite_float8(beta) THEN
    RAISE EXCEPTION 'provsql.beta: parameters must be finite (got alpha=%, beta=%)', alpha, beta;
  END IF;
  IF alpha <= 0 OR beta <= 0 THEN
    RAISE EXCEPTION 'provsql.beta: parameters must be strictly positive (got alpha=%, beta=%)', alpha, beta;
  END IF;
  IF alpha = 1 AND beta = 1 THEN
    RETURN provsql.uniform(0, 1);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'beta:' || alpha || ',' || beta);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct a Binomial(n, p) random variable (number of
 *        successes in @p n independent trials), as a categorical
 *
 * Enumerated over <tt>{0..n}</tt> by the log-space recurrence
 * <tt>ln p(k+1) = ln p(k) + ln((n-k)/(k+1)) + ln(p/(1-p))</tt>
 * (outcomes below 1e-15 relative mass are dropped).  @c p = 0 /
 * @c p = 1 are Diracs at @c 0 / @c n; @c n is capped at 10000.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Binomial_distribution">Wikipedia: Binomial distribution</a>
 */
CREATE OR REPLACE FUNCTION binomial(n integer, p double precision)
  RETURNS random_variable AS
$$
DECLARE
  outcomes double precision[] := '{}';
  lps double precision[] := '{}';
  lp double precision;
  k int;
BEGIN
  IF n IS NULL OR n < 0 THEN
    RAISE EXCEPTION 'provsql.binomial: n must be non-negative (got %)', n;
  END IF;
  IF NOT provsql.is_finite_float8(p) OR p < 0 OR p > 1 THEN
    RAISE EXCEPTION 'provsql.binomial: p must be in [0, 1] (got %)', p;
  END IF;
  IF n > 10000 THEN
    RAISE EXCEPTION 'provsql.binomial: n = % exceeds 10000; approximate with normal(n*p, sqrt(n*p*(1-p)))', n;
  END IF;
  IF n = 0 OR p = 0 THEN
    RETURN provsql.as_random(0);
  END IF;
  IF p = 1 THEN
    RETURN provsql.as_random(n);
  END IF;
  lp := n * ln(1 - p);   -- ln p(0)
  outcomes := array_append(outcomes, 0::double precision);
  lps := array_append(lps, lp);
  FOR k IN 0..(n - 1) LOOP
    lp := lp + ln((n - k)::double precision / (k + 1)) + ln(p / (1 - p));
    outcomes := array_append(outcomes, (k + 1)::double precision);
    lps := array_append(lps, lp);
  END LOOP;
  RETURN provsql.categorical_from_log_pmf(outcomes, lps);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct a Geometric(p) random variable -- the number of
 *        TRIALS up to and including the first success (support
 *        starting at 1; subtract 1 for the failures convention)
 *
 * <tt>P(X = k) = (1-p)^{k-1} p</tt>, enumerated up to the 1e-15
 * relative-mass tail and renormalised.  @c p = 1 is a Dirac at @c 1.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Geometric_distribution">Wikipedia: Geometric distribution</a>
 */
CREATE OR REPLACE FUNCTION geometric(p double precision)
  RETURNS random_variable AS
$$
DECLARE
  k_max int;
  outcomes double precision[] := '{}';
  lps double precision[] := '{}';
  k int;
BEGIN
  IF NOT provsql.is_finite_float8(p) OR p <= 0 OR p > 1 THEN
    RAISE EXCEPTION 'provsql.geometric: p must be in (0, 1] (got %)', p;
  END IF;
  IF p = 1 THEN
    RETURN provsql.as_random(1);
  END IF;
  k_max := 1 + ceil(ln(1e-15) / ln(1 - p))::int;
  IF k_max > 10000 THEN
    RAISE EXCEPTION 'provsql.geometric: support window of % outcomes exceeds 10000 (p = % is too small); approximate with exponential(%)', k_max, p, p;
  END IF;
  FOR k IN 1..k_max LOOP
    outcomes := array_append(outcomes, k::double precision);
    lps := array_append(lps, (k - 1) * ln(1 - p) + ln(p));
  END LOOP;
  RETURN provsql.categorical_from_log_pmf(outcomes, lps);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct a Hypergeometric(N, K, n) random variable: the
 *        number of marked items among @p n draws WITHOUT replacement
 *        from a population of @p pop_n items of which @p k_marked are
 *        marked
 *
 * The exact finite support <tt>[max(0, n-(N-K)), min(n, K)]</tt> is
 * enumerated by the pmf ratio recurrence (in log space, so large
 * populations cannot overflow) and normalised -- exact "sampling
 * without replacement" probabilities with no combinatorial functions
 * needed.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Hypergeometric_distribution">Wikipedia: Hypergeometric distribution</a>
 */
CREATE OR REPLACE FUNCTION hypergeometric(pop_n integer, k_marked integer, n integer)
  RETURNS random_variable AS
$$
DECLARE
  lo int;
  hi int;
  outcomes double precision[] := '{}';
  lps double precision[] := '{}';
  lp double precision := 0;   -- relative log-mass; normalised later
  k int;
BEGIN
  IF pop_n IS NULL OR k_marked IS NULL OR n IS NULL
     OR pop_n < 0 OR k_marked < 0 OR n < 0
     OR k_marked > pop_n OR n > pop_n THEN
    RAISE EXCEPTION 'provsql.hypergeometric: need 0 <= k_marked, n <= pop_n (got pop_n=%, k_marked=%, n=%)', pop_n, k_marked, n;
  END IF;
  lo := greatest(0, n - (pop_n - k_marked));
  hi := least(n, k_marked);
  IF hi - lo + 1 > 10000 THEN
    RAISE EXCEPTION 'provsql.hypergeometric: support window of % outcomes exceeds 10000', hi - lo + 1;
  END IF;
  outcomes := array_append(outcomes, lo::double precision);
  lps := array_append(lps, lp);
  FOR k IN lo..(hi - 1) LOOP
    -- pmf(k+1)/pmf(k) = (K-k)(n-k) / ((k+1)(N-K-n+k+1))
    lp := lp + ln((k_marked - k)::double precision * (n - k))
             - ln((k + 1)::double precision * (pop_n - k_marked - n + k + 1));
    outcomes := array_append(outcomes, (k + 1)::double precision);
    lps := array_append(lps, lp);
  END LOOP;
  RETURN provsql.categorical_from_log_pmf(outcomes, lps);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Construct a negative-binomial random variable: the number of
 *        FAILURES before the @p r-th success (support starting at 0),
 *        with real @p r > 0 allowed (the Polya / overdispersed-count
 *        parameterisation, the Poisson-Gamma mixture)
 *
 * <tt>P(X = k) = C(k+r-1, k) p^r (1-p)^k</tt>, enumerated by the
 * log-space recurrence
 * <tt>ln p(k+1) = ln p(k) + ln((k+r)/(k+1)) + ln(1-p)</tt> up to the
 * 1e-15 relative-mass tail.  @c p = 1 is a Dirac at @c 0.
 *
 * @sa <a href="https://en.wikipedia.org/wiki/Negative_binomial_distribution">Wikipedia: Negative binomial distribution</a>
 */
CREATE OR REPLACE FUNCTION negative_binomial(r double precision, p double precision)
  RETURNS random_variable AS
$$
DECLARE
  outcomes double precision[] := '{}';
  lps double precision[] := '{}';
  lp double precision;
  max_lp double precision;
  mean double precision;
  k int := 0;
BEGIN
  IF NOT provsql.is_finite_float8(r) OR r <= 0 THEN
    RAISE EXCEPTION 'provsql.negative_binomial: r must be finite and strictly positive (got %)', r;
  END IF;
  IF NOT provsql.is_finite_float8(p) OR p <= 0 OR p > 1 THEN
    RAISE EXCEPTION 'provsql.negative_binomial: p must be in (0, 1] (got %)', p;
  END IF;
  IF p = 1 THEN
    RETURN provsql.as_random(0);
  END IF;
  mean := r * (1 - p) / p;
  lp := r * ln(p);   -- ln p(0)
  max_lp := lp;
  outcomes := array_append(outcomes, 0::double precision);
  lps := array_append(lps, lp);
  LOOP
    lp := lp + ln((k + r) / (k + 1)) + ln(1 - p);
    k := k + 1;
    IF lp > max_lp THEN max_lp := lp; END IF;
    outcomes := array_append(outcomes, k::double precision);
    lps := array_append(lps, lp);
    EXIT WHEN k > mean AND lp < max_lp + ln(1e-15);
    IF k >= 10000 THEN
      RAISE EXCEPTION 'provsql.negative_binomial: support window exceeds 10000 outcomes (r=%, p=%)', r, p;
    END IF;
  END LOOP;
  RETURN provsql.categorical_from_log_pmf(outcomes, lps);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Catalog of the registered continuous-distribution families.
 *
 * One row per @c gate_rv family known to this build of the extension:
 * @c name is the on-disk token (the part before the colon in the gate's
 * @c extra encoding), @c nparams the parameter count, @c param_names the
 * conventional parameter symbols in @c extra order (e.g.
 * <tt>{μ, σ}</tt>), and @c label a short display glyph (e.g. @c "N",
 * @c "Γ").  UI clients (ProvSQL Studio's circuit inspector) read this to
 * render families they were not hard-coded for, so a newly added family
 * shows up without a client release.
 */
CREATE OR REPLACE FUNCTION rv_families()
  RETURNS TABLE(name TEXT, nparams INT, param_names TEXT[], label TEXT) AS
  'provsql','rv_families' LANGUAGE C STABLE PARALLEL SAFE;

/**
 * @brief Gaussian-mixture-model (GMM) constructor.
 *
 * Packages the common fitted-density pattern -- a categorical choice
 * among Normal components -- into one call:
 *
 * @code
 * provsql.gmm(weights => ARRAY[0.3, 0.5, 0.2],
 *             means   => ARRAY[120.0, 380.0, 1200.0],
 *             stddevs => ARRAY[40.0, 90.0, 250.0])
 * @endcode
 *
 * No new gate: the mixture decomposes into a stick-breaking cascade of
 * Bernoulli @c gate_mixture nodes over @c gate_rv Normal leaves
 * (component @c i is selected with conditional probability
 * @c w_i / (w_i + ... + w_n), so the joint selection probabilities are
 * exactly @p weights), which every evaluator already handles: moments
 * are closed-form through the mixture recursion, sampling is exact,
 * and comparisons ride the existing mixture machinery.  Zero-weight
 * components are skipped; a single positive-weight component returns
 * its Normal directly (no mixture node).
 *
 * Validation mirrors @c categorical: same-length non-empty arrays,
 * weights finite in <tt>[0, 1]</tt> summing to 1 within @c 1e-9; the
 * component parameters are validated by @c provsql.normal (finite
 * @c mu, non-negative @c sigma; @c sigma @c = @c 0 degenerates to a
 * Dirac component).
 *
 * @sa @c mixture, @c categorical, @c normal
 * @sa <a href="https://en.wikipedia.org/wiki/Mixture_model">Wikipedia: Mixture model</a>
 */
CREATE OR REPLACE FUNCTION gmm(
  weights double precision[],
  means   double precision[],
  stddevs double precision[])
  RETURNS random_variable AS
$$
DECLARE
  n integer;
  w_sum double precision := 0.0;
  i integer;
  acc random_variable := NULL;
  remaining double precision := 0.0;
BEGIN
  IF weights IS NULL OR means IS NULL OR stddevs IS NULL THEN
    RAISE EXCEPTION 'provsql.gmm: weights, means, and stddevs must be non-null';
  END IF;
  n := array_length(weights, 1);
  IF n IS NULL OR n < 1 THEN
    RAISE EXCEPTION 'provsql.gmm: weights must be non-empty';
  END IF;
  IF array_length(means, 1) <> n OR array_length(stddevs, 1) <> n THEN
    RAISE EXCEPTION 'provsql.gmm: weights, means, and stddevs must have the same length (got %, %, %)',
      n, array_length(means, 1), array_length(stddevs, 1);
  END IF;
  FOR i IN 1..n LOOP
    IF weights[i] IS NULL OR weights[i] = 'NaN'::float8
       OR weights[i] < 0 OR weights[i] > 1 THEN
      RAISE EXCEPTION 'provsql.gmm: weights[%] must be in [0,1] (got %)',
        i, weights[i];
    END IF;
    w_sum := w_sum + weights[i];
  END LOOP;
  IF abs(w_sum - 1.0) > 1e-9 THEN
    RAISE EXCEPTION 'provsql.gmm: weights must sum to 1 within 1e-9 (got %)', w_sum;
  END IF;

  -- Stick-breaking, built back to front: acc holds the mixture of
  -- components i+1..n, and prepending component i selects it with
  -- conditional probability w_i / (w_i + ... + w_n).
  FOR i IN REVERSE n..1 LOOP
    IF weights[i] <= 0.0 THEN
      CONTINUE;
    END IF;
    IF acc IS NULL THEN
      acc := provsql.normal(means[i], stddevs[i]);
      remaining := weights[i];
    ELSE
      remaining := remaining + weights[i];
      acc := provsql.mixture(least(1.0, weights[i] / remaining),
                             provsql.normal(means[i], stddevs[i]), acc);
    END IF;
  END LOOP;
  RETURN acc;
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Empirical-samples constructor: the ecdf of a sample bundle as
 *        a @c random_variable.
 *
 * Loads a Monte Carlo / MCMC / bootstrap sample array as the discrete
 * distribution putting mass @c 1/n on each draw (duplicates merge, so a
 * value drawn @c k times carries @c k/n) -- the standard empirical
 * distribution.  Reduces entirely to @ref categorical, so the whole
 * exact discrete surface applies: moments are the sample moments,
 * comparisons against constants are decided analytically ("fraction of
 * samples below c"), and quantiles are the exact empirical quantiles.
 *
 * @code
 * -- Bulk load via array_agg over a sample table
 * INSERT INTO model_posteriors
 * SELECT param, provsql.empirical_samples(array_agg(value))
 * FROM mcmc_chain GROUP BY param;
 * @endcode
 *
 * At most 10000 distinct values (the categorical block cap): thin the
 * chain or bin the samples (e.g. with @c width_bucket) beyond that.
 *
 * @sa @ref categorical, @ref empirical_cdf
 * @sa <a href="https://en.wikipedia.org/wiki/Empirical_distribution_function">Wikipedia: Empirical distribution function</a>
 */
CREATE OR REPLACE FUNCTION empirical_samples(samples double precision[])
  RETURNS random_variable AS
$$
DECLARE
  n integer;
  sorted double precision[];
  outcomes double precision[] := '{}';
  probs double precision[] := '{}';
  v double precision;
  prev double precision;
  run integer := 0;
  started boolean := false;
BEGIN
  n := array_length(samples, 1);
  IF n IS NULL OR n < 1 THEN
    RAISE EXCEPTION 'provsql.empirical_samples: samples must be non-empty';
  END IF;
  sorted := ARRAY(SELECT s FROM unnest(samples) AS s ORDER BY 1);
  FOREACH v IN ARRAY sorted LOOP
    IF v IS NULL OR v = 'NaN'::float8
       OR v = 'Infinity'::float8 OR v = '-Infinity'::float8 THEN
      RAISE EXCEPTION
        'provsql.empirical_samples: samples must be finite (got %)', v;
    END IF;
    IF started AND v = prev THEN
      run := run + 1;
    ELSE
      IF started THEN
        outcomes := outcomes || prev;
        probs := probs || (run::double precision / n);
      END IF;
      prev := v;
      run := 1;
      started := true;
    END IF;
  END LOOP;
  outcomes := outcomes || prev;
  probs := probs || (run::double precision / n);
  IF array_length(outcomes, 1) > 10000 THEN
    RAISE EXCEPTION
      'provsql.empirical_samples: at most 10000 distinct values are '
      'supported (got %); thin the chain or bin the samples (e.g. with '
      'width_bucket)', array_length(outcomes, 1);
  END IF;
  RETURN provsql.categorical(probs, outcomes);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief Empirical-CDF constructor: a piecewise-linear CDF table as a
 *        @c random_variable.
 *
 * Loads a tabulated CDF -- simulation output percentile tables, risk
 * models, expert-elicited forecasts -- as the distribution whose CDF is
 * @c cdf[i] at @c grid[i], linear in between: mass
 * @c cdf[i+1] @c - @c cdf[i] spread uniformly over
 * <tt>(grid[i], grid[i+1])</tt>, plus (when @c cdf[1] @c > @c 0) an
 * atom of mass @c cdf[1] at @c grid[1] for the probability at or below
 * the grid start.  Packaged, like @ref gmm, as a stick-breaking cascade
 * of Bernoulli @ref mixture nodes over @ref uniform components (and the
 * optional @ref as_random atom), so moments and sampling are exact
 * through the existing mixture machinery; comparisons ride Monte Carlo.
 *
 * @code
 * provsql.empirical_cdf(
 *   grid => ARRAY[0.0, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0],
 *   cdf  => ARRAY[0.32, 0.51, 0.67, 0.82, 0.94, 0.99, 1.0])
 * @endcode
 *
 * Validation: same-length arrays of at least two entries, @p grid
 * strictly increasing and finite, @p cdf non-decreasing within
 * <tt>[0, 1]</tt> and ending at @c 1 within @c 1e-9.
 *
 * @sa @ref gmm, @ref empirical_samples
 * @sa <a href="https://en.wikipedia.org/wiki/Cumulative_distribution_function">Wikipedia: Cumulative distribution function</a>
 */
CREATE OR REPLACE FUNCTION empirical_cdf(grid double precision[],
                                         cdf double precision[])
  RETURNS random_variable AS
$$
DECLARE
  n integer;
  i integer;
  acc random_variable := NULL;
  remaining double precision := 0.0;
  w double precision;
  comp random_variable;
BEGIN
  n := array_length(grid, 1);
  IF n IS NULL OR n < 2 THEN
    RAISE EXCEPTION 'provsql.empirical_cdf: grid must have at least two entries';
  END IF;
  IF array_length(cdf, 1) <> n THEN
    RAISE EXCEPTION 'provsql.empirical_cdf: grid and cdf must have the same length (got % and %)',
      n, array_length(cdf, 1);
  END IF;
  IF n > 10000 THEN
    RAISE EXCEPTION 'provsql.empirical_cdf: at most 10000 grid points are supported (got %)', n;
  END IF;
  FOR i IN 1..n LOOP
    IF grid[i] IS NULL OR grid[i] = 'NaN'::float8
       OR grid[i] = 'Infinity'::float8 OR grid[i] = '-Infinity'::float8 THEN
      RAISE EXCEPTION 'provsql.empirical_cdf: grid[%] must be finite (got %)', i, grid[i];
    END IF;
    IF i > 1 AND NOT grid[i] > grid[i-1] THEN
      RAISE EXCEPTION 'provsql.empirical_cdf: grid must be strictly increasing (grid[%] = %, grid[%] = %)',
        i-1, grid[i-1], i, grid[i];
    END IF;
    IF cdf[i] IS NULL OR cdf[i] = 'NaN'::float8 OR cdf[i] < 0 OR cdf[i] > 1 THEN
      RAISE EXCEPTION 'provsql.empirical_cdf: cdf[%] must be in [0,1] (got %)', i, cdf[i];
    END IF;
    IF i > 1 AND cdf[i] < cdf[i-1] THEN
      RAISE EXCEPTION 'provsql.empirical_cdf: cdf must be non-decreasing (cdf[%] = %, cdf[%] = %)',
        i-1, cdf[i-1], i, cdf[i];
    END IF;
  END LOOP;
  IF abs(cdf[n] - 1.0) > 1e-9 THEN
    RAISE EXCEPTION 'provsql.empirical_cdf: cdf must end at 1 within 1e-9 (got %)', cdf[n];
  END IF;

  -- Stick-breaking cascade, back to front: component i = 1 is the atom
  -- at the grid start (mass cdf[1]); component i >= 2 is
  -- uniform(grid[i-1], grid[i]) with mass cdf[i] - cdf[i-1].
  FOR i IN REVERSE n..1 LOOP
    w := CASE WHEN i = 1 THEN cdf[1] ELSE cdf[i] - cdf[i-1] END;
    IF w <= 0.0 THEN
      CONTINUE;
    END IF;
    comp := CASE WHEN i = 1 THEN provsql.as_random(grid[1])
                 ELSE provsql.uniform(grid[i-1], grid[i]) END;
    IF acc IS NULL THEN
      acc := comp;
      remaining := w;
    ELSE
      remaining := remaining + w;
      acc := provsql.mixture(least(1.0, w / remaining), comp, acc);
    END IF;
  END LOOP;
  RETURN acc;
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

/**
 * @brief @c random_variable ^ @c random_variable (gate_arith POW).
 *
 * Real-valued branch only: evaluation raises if a negative base is
 * drawn together with a non-integer exponent (write
 * <tt>pow(greatest(x, 0), p)</tt> for the non-negative branch).
 */
CREATE OR REPLACE FUNCTION random_variable_pow(
  a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      7,  -- PROVSQL_ARITH_POW
      ARRAY[(a)::uuid,
            (b)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Natural logarithm of a @c random_variable (gate_arith LN).
 *
 * Defined on @c [0, +Infinity): evaluation raises if a negative value
 * is drawn (restrict the argument's support); a draw of exactly @c 0
 * yields @c -Infinity.
 */
CREATE OR REPLACE FUNCTION ln(a random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      8,  -- PROVSQL_ARITH_LN
      ARRAY[(a)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief @c e^x for a @c random_variable (gate_arith EXP).  Total. */
CREATE OR REPLACE FUNCTION exp(a random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(
    provsql.provenance_arith(
      9,  -- PROVSQL_ARITH_EXP
      ARRAY[(a)::uuid]));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief @c pow / @c power spellings of the @c ^ operator, mirroring
 *        PostgreSQL's numeric surface.  Scalar exponents resolve
 *        through the implicit numeric-to-rv casts:
 *        <tt>pow(x, 0.5)</tt> is <tt>x ^ 0.5</tt>.
 */
CREATE OR REPLACE FUNCTION pow(a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_pow(a, b);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION power(a random_variable, b random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_pow(a, b);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Square root of a @c random_variable: sugar for
 *        <tt>x ^ 0.5</tt> (no gate or opcode of its own).  Evaluation
 *        raises on a negative draw, like any non-integer exponent.
 */
CREATE OR REPLACE FUNCTION sqrt(a random_variable)
  RETURNS random_variable AS
$$
  SELECT provsql.random_variable_pow(a, provsql.as_random(0.5));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o
      JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '^'
      AND o.oprleft = 'provsql.random_variable'::regtype::oid
      AND o.oprright = 'provsql.random_variable'::regtype::oid
      AND o.oprcode <> 0
  ) THEN
    CREATE OPERATOR ^ (
      LEFTARG    = random_variable,
      RIGHTARG   = random_variable,
      PROCEDURE  = random_variable_pow
    );
  END IF;
END $$;

/**
 * @brief btree comparison support for @c random_variable -- always an error.
 *
 * A @c random_variable is a distribution, not a scalar, so it has no total
 * order: sorting (@c ORDER @c BY), de-duplicating (@c DISTINCT), grouping, and
 * the built-in @c GREATEST / @c LEAST all reduce to this btree comparison
 * proc, which raises a clear diagnostic rather than a placeholder message.
 *
 * The proc exists only so a DEFAULT btree operator class can be declared for
 * @c random_variable -- which is what lets PostgreSQL's @c GREATEST / @c LEAST
 * grammar parse over random variables so the planner hook can lift it into a
 * @c gate_arith @c MAX / @c MIN order statistic.  When the hook is active the
 * @c GREATEST / @c LEAST node is rewritten before it ever calls this proc.
 */
CREATE OR REPLACE FUNCTION random_variable_btree_cmp(
  a random_variable, b random_variable) RETURNS integer AS
$$
BEGIN
  RAISE EXCEPTION 'comparison or ordering of random_variable values is '
                  'meaningless: a random_variable is a distribution, not a scalar'
    USING HINT =
      'Compare them as a probabilistic event -- in a WHERE / JOIN clause or '
      'with probability(x > y); take order statistics with provsql.greatest / '
      'provsql.least (or the min / max aggregates); summarise numerically with '
      'expected / variance / support.';
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_opclass c
      JOIN pg_namespace n ON n.oid = c.opcnamespace
      JOIN pg_am am ON am.oid = c.opcmethod
    WHERE n.nspname = 'provsql' AND c.opcname = 'random_variable_ops'
      AND am.amname = 'btree'
  ) THEN
    CREATE OPERATOR CLASS random_variable_ops
      DEFAULT FOR TYPE random_variable USING btree AS
        OPERATOR 1 <,
        OPERATOR 2 <=,
        OPERATOR 3 =,
        OPERATOR 4 >=,
        OPERATOR 5 >,
        FUNCTION 1 random_variable_btree_cmp(random_variable, random_variable);
  END IF;
END $$;

/**
 * @name Order statistics over random_variable
 *
 * Same-row @c greatest / @c least over @c random_variable arguments: the
 * order-statistic counterpart of the element-wise @c "+ - * /" operators.
 * They lower to a single @c gate_arith with the @c MAX / @c MIN opcode over
 * the argument circuits, the same n-ary shape the @c max / @c min aggregates
 * build.  Evaluation is Monte-Carlo-correct out of the box (@c std::max /
 * @c std::min over the jointly-sampled children, so shared base RVs stay
 * coupled); closed forms for i.i.d. families come from the analytic
 * order-statistic pass.
 *
 * PostgreSQL's built-in @c GREATEST / @c LEAST are dedicated syntax (a
 * @c MinMaxExpr requiring a btree comparison), not overloadable functions, so
 * the surface is the schema-qualified @c provsql.greatest(...) /
 * @c provsql.least(...).  @c NULL arguments are ignored, matching the built-in
 * (an all-@c NULL / empty call returns @c NULL).
 * @{
 */

-- "greatest" / "least" are col_name keywords, so the CREATE FUNCTION name
-- must be quoted; callers reach them qualified as provsql.greatest(...).
-- Idempotence: max / min ignore repeats, so identical children (same gate)
-- are de-duplicated -- greatest(x, x, y) == greatest(x, y) -- and a single
-- surviving child collapses to itself -- greatest(x) == x.  DISTINCT also sorts
-- the children, so the argument order does not matter for gate sharing.  (Two
-- independent draws of the same distribution are distinct gates and are NOT
-- de-duplicated.)
CREATE OR REPLACE FUNCTION "greatest"(VARIADIC args random_variable[])
  RETURNS random_variable AS
$$
DECLARE
  children uuid[];
BEGIN
  IF args IS NULL THEN
    RETURN NULL;
  END IF;
  SELECT array_agg(DISTINCT (a)::uuid) INTO children
    FROM unnest(args) a WHERE a IS NOT NULL;
  IF children IS NULL OR array_length(children, 1) IS NULL THEN
    RETURN NULL;
  END IF;
  IF array_length(children, 1) = 1 THEN
    RETURN provsql.random_variable_make(children[1]);
  END IF;
  RETURN provsql.random_variable_make(
    provsql.provenance_arith(5, children));  -- 5 = PROVSQL_ARITH_MAX
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION "least"(VARIADIC args random_variable[])
  RETURNS random_variable AS
$$
DECLARE
  children uuid[];
BEGIN
  IF args IS NULL THEN
    RETURN NULL;
  END IF;
  SELECT array_agg(DISTINCT (a)::uuid) INTO children
    FROM unnest(args) a WHERE a IS NOT NULL;
  IF children IS NULL OR array_length(children, 1) IS NULL THEN
    RETURN NULL;
  END IF;
  IF array_length(children, 1) = 1 THEN
    RETURN provsql.random_variable_make(children[1]);
  END IF;
  RETURN provsql.random_variable_make(
    provsql.provenance_arith(6, children));  -- 6 = PROVSQL_ARITH_MIN
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

/**
 * @brief Build a @c random_variable from a guarded-selection @c gate_case.
 *
 * Thin @c random_variable wrapper over @c provenance_case (defined with the
 * other gate builders, since it is uuid-only), the target of the planner-hook
 * @c CASE-over-RV rewrite: the hook flattens the branches into
 * @c [guard_1, value_1, ..., default] and emits this call so an RV-typed
 * @c CASE surfaces as a first-class @c random_variable.
 */
CREATE OR REPLACE FUNCTION rv_case(
  children UUID[]
)
RETURNS random_variable AS
$$
  SELECT provsql.random_variable_make(provsql.provenance_case(children));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Build an @c agg_token from a guarded-selection @c gate_case.
 *
 * The aggregate-carrier analogue of @c rv_case: a thin @c agg_token wrapper
 * over the carrier-agnostic @c provenance_case, the target of the planner-hook
 * lowering of a searched @c CASE whose guards are aggregate comparisons and
 * whose branches are aggregates. The branches (and default) are already
 * flattened into @c [guard_1, value_1, ..., default] UUIDs.  The display cell
 * carries the actual-world CASE value -- the branch selected on the actual
 * data, resolved by @c agg_gate_value, exactly as a bare aggregate's cell
 * carries its actual-world value.  The probabilistic result is produced by
 * the measure evaluators (``expected`` / ``probability`` / possible-worlds /
 * Monte Carlo) from the gate, not the token's cell.
 */
CREATE OR REPLACE FUNCTION agg_case(
  children UUID[]
)
RETURNS agg_token AS
$$
  SELECT provsql.agg_token_make(t, coalesce(provsql.agg_gate_value(t), 0))
  FROM (SELECT provsql.provenance_case(children) AS t) AS s;
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Identity-parameterised per-row wrap for an RV-returning aggregate.
 *
 * Generalises the two-argument @ref rv_aggregate_semimod: the else-branch
 * (a row's contribution when its provenance is false) is
 * @c as_random(@p identity) instead of the additive @c as_random(0).  The
 * planner-hook rewrite bakes each aggregate's own identity element into the
 * wrap -- @c 1 for @c product, @f$-\infty@f$ / @f$+\infty@f$ for @c max /
 * @c min -- so the aggregate's final function is a plain fold over the
 * per-row mixtures with no gate inspection.  @c sum keeps the two-argument
 * form (@c identity @c = @c 0).
 */
CREATE OR REPLACE FUNCTION rv_aggregate_semimod(
  prov uuid, rv random_variable, identity double precision)
  RETURNS random_variable AS
$$
  SELECT provsql.mixture(prov, rv, provsql.as_random(identity));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Per-row denominator wrap for @c avg(random_variable): the
 *        provenance indicator @f$\mathbf{1}\{\varphi\}@f$.
 *
 * The row contributes @c 1 to the running count when present and @c 0 when
 * absent, so @c sum over these wraps is the provenance-weighted count
 * @f$\sum_i \mathbf{1}\{\varphi_i\}@f$.  The planner-hook rewrites
 * @c avg(x) into @c rv_sum_or_null(rv_aggregate_semimod(prov, x)) @c /
 * @c sum(rv_aggregate_indicator(prov)) -- the "@c AVG @c = @c SUM @c /
 * @c COUNT" identity lifted into the @c random_variable algebra -- so
 * @c avg rides entirely on @c sum's fold and never inspects a gate.
 */
CREATE OR REPLACE FUNCTION rv_aggregate_indicator(prov uuid)
  RETURNS random_variable AS
$$
  SELECT provsql.rv_aggregate_semimod(prov, provsql.as_random(1::double precision));
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Numerator final function for the @c avg rewrite: @c sum, but
 *        @c NULL on an empty group.
 *
 * Identical to @ref sum_rv_ffunc except that an empty group returns
 * @c NULL rather than the additive identity @c as_random(0).  The
 * planner-hook @c avg rewrite emits
 * @c rv_sum_or_null(rv_aggregate_semimod(prov, x)) @c /
 * @c sum(rv_aggregate_indicator(prov)); @c random_variable_div is
 * @c STRICT, so an empty group propagates the numerator's @c NULL and
 * @c avg is @c NULL -- the standard SQL @c AVG convention -- while a
 * non-empty group behaves exactly like @c sum.
 */
CREATE OR REPLACE FUNCTION rv_sum_or_null_ffunc(state uuid[])
  RETURNS random_variable AS
$$
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN NULL;
  END IF;
  IF array_length(state, 1) = 1 THEN
    RETURN provsql.random_variable_make(state[1]);
  END IF;
  RETURN provsql.random_variable_make(
    provsql.provenance_arith(0, state));  -- 0 = PROVSQL_ARITH_PLUS
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'rv_sum_or_null'
      AND p.pronargs = 1
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE rv_sum_or_null(random_variable) (
      SFUNC     = sum_rv_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = rv_sum_or_null_ffunc
    );
  END IF;
END $$;

/**
 * @brief Final function for @c avg(random_variable).
 *
 * @c avg lifts the "@c AVG @c = @c SUM @c / @c COUNT" identity into the
 * @c random_variable algebra:
 * @f[
 *   \mathrm{AVG}(x) \;=\; \frac{\sum_i \mathbf{1}\{\varphi_i\} \cdot X_i}
 *                                {\sum_i \mathbf{1}\{\varphi_i\}}.
 * @f]
 * In a provenance-tracked query the planner-hook rewrites @c avg(x) into
 * @c rv_sum_or_null(rv_aggregate_semimod(prov, x)) @c /
 * @c sum(rv_aggregate_indicator(prov)) (see
 * @c make_rv_aggregate_expression), so both the numerator and the
 * provenance-weighted count denominator are built by @c sum's fold and no
 * gate is inspected.  This FFUNC is therefore reached only on an
 * @em untracked call, where every row is unconditionally present: the
 * numerator is @c sum over the raw per-row RVs and the denominator is the
 * plain row count @c n (each row contributing @c as_random(1)).
 *
 * Empty group: returns @c NULL, matching standard SQL @c AVG (and unlike
 * @c sum, whose empty group is the additive identity @c as_random(0)):
 * the caller cannot otherwise disambiguate "0 rows" from "rows summing
 * to 0".
 */
CREATE OR REPLACE FUNCTION avg_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  n integer;
  i integer;
  num_token uuid;
  denom_token uuid;
  denom_state uuid[] := '{}';
  one_uuid uuid;
BEGIN
  IF state IS NULL THEN
    RETURN NULL;
  END IF;
  n := array_length(state, 1);
  IF n IS NULL THEN
    RETURN NULL;
  END IF;

  one_uuid := (provsql.as_random(1::double precision))::uuid;
  FOR i IN 1..n LOOP
    denom_state := array_append(denom_state, one_uuid);
  END LOOP;

  IF n = 1 THEN
    num_token := state[1];
    denom_token := denom_state[1];
  ELSE
    num_token := provsql.provenance_arith(0, state);          -- 0 = PLUS
    denom_token := provsql.provenance_arith(0, denom_state);  -- 0 = PLUS
  END IF;

  RETURN provsql.random_variable_make(
    provsql.provenance_arith(
      3,  -- 3 = PROVSQL_ARITH_DIV
      ARRAY[num_token, denom_token]));
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

/**
 * @brief Final function for @c product(random_variable): fold a
 *        @c gate_arith TIMES root over the per-row contributions.
 *
 * Multiplicative analogue of @c sum(random_variable):
 * @f[
 *   \mathrm{PRODUCT}(x) \;=\; \prod_i \big(\mathbf{1}\{\varphi_i\} \cdot X_i
 *                                          + \mathbf{1}\{\neg\varphi_i\} \cdot 1\big)
 *                       \;=\; \prod_{i : \varphi_i} X_i.
 * @f]
 * Each per-row contribution already carries the multiplicative identity
 * as its absent-row value: a provenance-tracked query wraps the argument
 * as @c mixture(prov_i, X_i, as_random(1)) (identity baked in by the
 * three-argument @ref rv_aggregate_semimod), and an untracked call passes
 * the raw RV through.  So the FFUNC is a plain fold with no gate
 * inspection: @c gate_arith(TIMES, state).
 *
 * Reuses @c sum_rv_sfunc as the state-transition function.  Empty group:
 * the multiplicative identity @c as_random(1) -- the counterpart to
 * @c sum's empty-group @c as_random(0).  Singleton group: the single
 * child directly, without a one-child TIMES root.
 */
CREATE OR REPLACE FUNCTION product_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN provsql.as_random(1::double precision);
  END IF;
  IF array_length(state, 1) = 1 THEN
    RETURN provsql.random_variable_make(state[1]);
  END IF;
  RETURN provsql.random_variable_make(
    provsql.provenance_arith(1, state));  -- 1 = PROVSQL_ARITH_TIMES
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

/**
 * @brief Final function for @c max(random_variable) / @c min(random_variable):
 *        fold a @c gate_arith MAX / MIN root over the per-row contributions.
 *
 * The order-statistic analogues of @c sum / @c product:
 * @f[
 *   \mathrm{MAX}(x) = \max_{i : \varphi_i} X_i, \qquad
 *   \mathrm{MIN}(x) = \min_{i : \varphi_i} X_i.
 * @f]
 * A row absent in a world (its provenance @f$\varphi_i@f$ false) must not
 * perturb the extremum, so it contributes the order-statistic identity
 * @f$\mp\infty@f$.  That identity is baked into each per-row contribution
 * upstream: a provenance-tracked query wraps the argument as
 * @c mixture(prov_i, X_i, as_random(∓∞)) (via the three-argument
 * @ref rv_aggregate_semimod), and an untracked call passes the raw RV
 * through.  So the FFUNC is a plain fold with no gate inspection:
 * @c gate_arith(@p op, state).
 *
 * Empty group: the identity @c as_random(@p identity) (@f$-\infty@f$ /
 * @f$+\infty@f$), the extremum counterpart to @c sum's @c as_random(0).
 * Singleton group: the single child directly.
 */
CREATE OR REPLACE FUNCTION extremum_rv_ffunc(
  state uuid[], op integer, identity double precision)
  RETURNS random_variable AS
$$
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN provsql.as_random(identity);
  END IF;
  IF array_length(state, 1) = 1 THEN
    RETURN provsql.random_variable_make(state[1]);
  END IF;
  RETURN provsql.random_variable_make(
    provsql.provenance_arith(op, state));
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION max_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
  -- 5 = PROVSQL_ARITH_MAX; empty-group / row-absent identity -inf.
  SELECT provsql.extremum_rv_ffunc(state, 5, '-Infinity'::double precision);
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION min_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
  -- 6 = PROVSQL_ARITH_MIN; empty-group / row-absent identity +inf.
  SELECT provsql.extremum_rv_ffunc(state, 6, 'Infinity'::double precision);
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'max'
      AND p.pronargs = 1
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE max(random_variable) (
      SFUNC     = sum_rv_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = max_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'min'
      AND p.pronargs = 1
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE min(random_variable) (
      SFUNC     = sum_rv_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = min_rv_ffunc
    );
  END IF;
END $$;

-- ---------------------------------------------------------------------
-- SQL-standard statistic aggregates over random_variable rows:
-- covar_pop / covar_samp / corr (two-argument), stddev_pop / stddev_samp
-- (one-argument), and the ordered-set percentile_cont.
--
-- Row presence is carried by a per-row 0/1 indicator RV: the public
-- aggregates use the certain indicator as_random(1) (every row present),
-- and a provenance-tracked query is rewritten by the planner hook
-- (make_rv_aggregate_expression) to the rv_*_impl aggregates whose extra
-- leading argument is rv_aggregate_indicator(prov), so a row absent in a
-- world drops out of every sum, the count, and the percentile member set.
-- The moment statistics are built from indicator-weighted power sums with
-- existing gate_arith opcodes (e.g. covar_pop = SXY/N - (SX/N)(SY/N)); a
-- world where the statistic is undefined (N = 0, or N = 1 for the sample
-- forms) evaluates to NaN, the established undefined-world convention the
-- moment estimators skip.  percentile_cont is the one gate the arithmetic
-- cannot express: it mints the PROVSQL_ARITH_PERCENTILE gate_arith
-- (interleaved [ind_1, x_1, ...] wires, fraction in extra) that the Monte
-- Carlo sampler evaluates by sorting each draw's present values and
-- interpolating.
-- ---------------------------------------------------------------------

/** @brief State transition for the one-argument RV statistic aggregates
 *  (@c stddev_pop / @c stddev_samp): append the certain indicator and the
 *  row's RV as a pair.  NULL rows are skipped (standard SQL). */
CREATE OR REPLACE FUNCTION rv_stat1_sfunc(state uuid[], x random_variable)
  RETURNS uuid[] AS
$$
  SELECT CASE
    WHEN x IS NULL THEN state
    ELSE state || ARRAY[(provsql.as_random(1::double precision))::uuid,
                        (x)::uuid]
  END;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

/** @brief State transition for the two-argument RV statistic aggregates
 *  (@c covar_pop / @c covar_samp / @c corr): append the certain indicator
 *  and the row's RV pair as a triple.  Rows with either side NULL are
 *  skipped (standard SQL covariance semantics). */
CREATE OR REPLACE FUNCTION rv_stat2_sfunc(
  state uuid[], x random_variable, y random_variable)
  RETURNS uuid[] AS
$$
  SELECT CASE
    WHEN x IS NULL OR y IS NULL THEN state
    ELSE state || ARRAY[(provsql.as_random(1::double precision))::uuid,
                        (x)::uuid, (y)::uuid]
  END;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

/** @brief Indicator-carrying state transition for the one-argument
 *  @c rv_*_impl statistic aggregates: the planner-hook rewrite passes the
 *  row's provenance indicator @c rv_aggregate_indicator(prov) as @p ind. */
CREATE OR REPLACE FUNCTION rv_stat1_impl_sfunc(
  state uuid[], ind random_variable, x random_variable)
  RETURNS uuid[] AS
$$
  SELECT CASE
    WHEN x IS NULL THEN state
    ELSE state || ARRAY[coalesce((ind)::uuid,
                          (provsql.as_random(1::double precision))::uuid),
                        (x)::uuid]
  END;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

/** @brief Indicator-carrying state transition for the two-argument
 *  @c rv_*_impl statistic aggregates. */
CREATE OR REPLACE FUNCTION rv_stat2_impl_sfunc(
  state uuid[], ind random_variable, x random_variable, y random_variable)
  RETURNS uuid[] AS
$$
  SELECT CASE
    WHEN x IS NULL OR y IS NULL THEN state
    ELSE state || ARRAY[coalesce((ind)::uuid,
                          (provsql.as_random(1::double precision))::uuid),
                        (x)::uuid, (y)::uuid]
  END;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

/**
 * @brief Mint the indicator-weighted power-sum gates shared by the
 *        covariance / stddev final functions.
 *
 * @p state is the flat interleaved aggregate state -- pairs
 * @c [ind, x, ...] (@p stride 2) or triples @c [ind, x, y, ...]
 * (@p stride 3).  Emits @c gate_arith tokens for
 * @f$N = \sum_i \mathbf{1}_i@f$, @f$SX = \sum_i \mathbf{1}_i x_i@f$,
 * @f$SXX = \sum_i \mathbf{1}_i x_i^2@f$ and, at stride 3, @f$SY@f$,
 * @f$SXY@f$, @f$SYY@f$.  The per-row indicator gate is shared between
 * @f$N@f$ and every product it weighs, so the Monte Carlo per-iteration
 * cache keeps the row's presence coupled across all the sums (and a
 * repeated child @c [ind, x, x] reuses the same draw of @c x, giving
 * @f$x^2@f$, not two independent draws).
 */
CREATE OR REPLACE FUNCTION rv_stat_sum_tokens(
  state uuid[], stride integer,
  OUT n_tok uuid, OUT sx_tok uuid, OUT sxx_tok uuid,
  OUT sy_tok uuid, OUT sxy_tok uuid, OUT syy_tok uuid)
AS
$$
DECLARE
  nrows integer := coalesce(array_length(state, 1), 0) / stride;
  inds uuid[] := '{}';
  xs   uuid[] := '{}';
  xxs  uuid[] := '{}';
  ys   uuid[] := '{}';
  xys  uuid[] := '{}';
  yys  uuid[] := '{}';
  ind uuid;
  x uuid;
  y uuid;
BEGIN
  FOR i IN 1..nrows LOOP
    ind := state[(i-1) * stride + 1];
    x   := state[(i-1) * stride + 2];
    inds := array_append(inds, ind);
    xs   := array_append(xs,  provenance_arith(1, ARRAY[ind, x]));
    xxs  := array_append(xxs, provenance_arith(1, ARRAY[ind, x, x]));
    IF stride = 3 THEN
      y := state[(i-1) * stride + 3];
      ys  := array_append(ys,  provenance_arith(1, ARRAY[ind, y]));
      xys := array_append(xys, provenance_arith(1, ARRAY[ind, x, y]));
      yys := array_append(yys, provenance_arith(1, ARRAY[ind, y, y]));
    END IF;
  END LOOP;
  n_tok   := provenance_arith(0, inds);
  sx_tok  := provenance_arith(0, xs);
  sxx_tok := provenance_arith(0, xxs);
  IF stride = 3 THEN
    sy_tok  := provenance_arith(0, ys);
    sxy_tok := provenance_arith(0, xys);
    syy_tok := provenance_arith(0, yys);
  END IF;
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

/** @brief Population-variance gate @f$SXX/N - (SX/N)^2@f$ from the
 *  power-sum tokens. */
CREATE OR REPLACE FUNCTION rv_stat_var_pop_token(
  n_tok uuid, s_tok uuid, ss_tok uuid)
  RETURNS uuid AS
$$
  SELECT provsql.provenance_arith(2, ARRAY[
    provsql.provenance_arith(3, ARRAY[ss_tok, n_tok]),
    provsql.provenance_arith(1, ARRAY[
      provsql.provenance_arith(3, ARRAY[s_tok, n_tok]),
      provsql.provenance_arith(3, ARRAY[s_tok, n_tok])])]);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Sample-variance gate @f$(SXX - SX^2/N) / (N - 1)@f$ from the
 *  power-sum tokens (NaN in a world with @f$N \le 1@f$, the undefined-world
 *  convention). */
CREATE OR REPLACE FUNCTION rv_stat_var_samp_token(
  n_tok uuid, s_tok uuid, ss_tok uuid)
  RETURNS uuid AS
$$
  SELECT provsql.provenance_arith(3, ARRAY[
    provsql.provenance_arith(2, ARRAY[
      ss_tok,
      provsql.provenance_arith(3, ARRAY[
        provsql.provenance_arith(1, ARRAY[s_tok, s_tok]), n_tok])]),
    provsql.provenance_arith(2, ARRAY[
      n_tok, (provsql.as_random(1::double precision))::uuid])]);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief @f$\sqrt{\max(v, 0)}@f$ gate over a variance token: the max-clamp
 *  removes the tiny negative values float error can produce (variance is
 *  mathematically non-negative), so the POW domain guard never fires. */
CREATE OR REPLACE FUNCTION rv_stat_sqrt_token(v_tok uuid)
  RETURNS uuid AS
$$
  SELECT provsql.provenance_arith(7, ARRAY[
    provsql.provenance_arith(5, ARRAY[
      v_tok, (provsql.as_random(0::double precision))::uuid]),
    (provsql.as_random(0.5::double precision))::uuid]);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Population-covariance gate @f$SXY/N - (SX/N)(SY/N)@f$ from the
 *  power-sum tokens. */
CREATE OR REPLACE FUNCTION rv_stat_covar_pop_token(
  n_tok uuid, sx_tok uuid, sy_tok uuid, sxy_tok uuid)
  RETURNS uuid AS
$$
  SELECT provsql.provenance_arith(2, ARRAY[
    provsql.provenance_arith(3, ARRAY[sxy_tok, n_tok]),
    provsql.provenance_arith(1, ARRAY[
      provsql.provenance_arith(3, ARRAY[sx_tok, n_tok]),
      provsql.provenance_arith(3, ARRAY[sy_tok, n_tok])])]);
$$ LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

/** @brief Final function for @c covar_pop(random_variable, random_variable). */
CREATE OR REPLACE FUNCTION covar_pop_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  t record;
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN NULL;
  END IF;
  SELECT * INTO t FROM rv_stat_sum_tokens(state, 3);
  RETURN random_variable_make(
    rv_stat_covar_pop_token(t.n_tok, t.sx_tok, t.sy_tok, t.sxy_tok));
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

/** @brief Final function for @c covar_samp(random_variable, random_variable):
 *  @f$(SXY - SX\,SY/N) / (N-1)@f$. */
CREATE OR REPLACE FUNCTION covar_samp_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  t record;
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN NULL;
  END IF;
  SELECT * INTO t FROM rv_stat_sum_tokens(state, 3);
  RETURN random_variable_make(
    provenance_arith(3, ARRAY[
      provenance_arith(2, ARRAY[
        t.sxy_tok,
        provenance_arith(3, ARRAY[
          provenance_arith(1, ARRAY[t.sx_tok, t.sy_tok]), t.n_tok])]),
      provenance_arith(2, ARRAY[
        t.n_tok, (as_random(1::double precision))::uuid])]));
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

/** @brief Final function for @c corr(random_variable, random_variable):
 *  @f$\mathrm{covar\_pop} / \sqrt{\max(v_x v_y, 0)}@f$ (a zero-variance
 *  world divides to @f$\pm\infty@f$ / NaN, the undefined-world convention,
 *  matching SQL's NULL for a zero-stddev input). */
CREATE OR REPLACE FUNCTION corr_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  t record;
  vx uuid;
  vy uuid;
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN NULL;
  END IF;
  SELECT * INTO t FROM rv_stat_sum_tokens(state, 3);
  vx := rv_stat_var_pop_token(t.n_tok, t.sx_tok, t.sxx_tok);
  vy := rv_stat_var_pop_token(t.n_tok, t.sy_tok, t.syy_tok);
  RETURN random_variable_make(
    provenance_arith(3, ARRAY[
      rv_stat_covar_pop_token(t.n_tok, t.sx_tok, t.sy_tok, t.sxy_tok),
      rv_stat_sqrt_token(provenance_arith(1, ARRAY[vx, vy]))]));
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

/** @brief Final function for @c stddev_pop(random_variable). */
CREATE OR REPLACE FUNCTION stddev_pop_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  t record;
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN NULL;
  END IF;
  SELECT * INTO t FROM rv_stat_sum_tokens(state, 2);
  RETURN random_variable_make(
    rv_stat_sqrt_token(
      rv_stat_var_pop_token(t.n_tok, t.sx_tok, t.sxx_tok)));
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

/** @brief Final function for @c stddev_samp(random_variable). */
CREATE OR REPLACE FUNCTION stddev_samp_rv_ffunc(state uuid[])
  RETURNS random_variable AS
$$
DECLARE
  t record;
BEGIN
  IF state IS NULL OR array_length(state, 1) IS NULL THEN
    RETURN NULL;
  END IF;
  SELECT * INTO t FROM rv_stat_sum_tokens(state, 2);
  RETURN random_variable_make(
    rv_stat_sqrt_token(
      rv_stat_var_samp_token(t.n_tok, t.sx_tok, t.sxx_tok)));
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'covar_pop'
      AND p.pronargs = 2
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
      AND p.proargtypes[1] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE covar_pop(random_variable, random_variable) (
      SFUNC     = rv_stat2_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = covar_pop_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'covar_samp'
      AND p.pronargs = 2
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
      AND p.proargtypes[1] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE covar_samp(random_variable, random_variable) (
      SFUNC     = rv_stat2_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = covar_samp_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'corr'
      AND p.pronargs = 2
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
      AND p.proargtypes[1] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE corr(random_variable, random_variable) (
      SFUNC     = rv_stat2_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = corr_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'stddev_pop'
      AND p.pronargs = 1
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE stddev_pop(random_variable) (
      SFUNC     = rv_stat1_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = stddev_pop_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'stddev_samp'
      AND p.pronargs = 1
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE stddev_samp(random_variable) (
      SFUNC     = rv_stat1_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = stddev_samp_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'rv_covar_pop_impl'
      AND p.pronargs = 3
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
      AND p.proargtypes[1] = 'provsql.random_variable'::regtype::oid
      AND p.proargtypes[2] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE rv_covar_pop_impl(
      random_variable, random_variable, random_variable) (
      SFUNC     = rv_stat2_impl_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = covar_pop_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'rv_covar_samp_impl'
      AND p.pronargs = 3
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
      AND p.proargtypes[1] = 'provsql.random_variable'::regtype::oid
      AND p.proargtypes[2] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE rv_covar_samp_impl(
      random_variable, random_variable, random_variable) (
      SFUNC     = rv_stat2_impl_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = covar_samp_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'rv_corr_impl'
      AND p.pronargs = 3
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
      AND p.proargtypes[1] = 'provsql.random_variable'::regtype::oid
      AND p.proargtypes[2] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE rv_corr_impl(
      random_variable, random_variable, random_variable) (
      SFUNC     = rv_stat2_impl_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = corr_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'rv_stddev_pop_impl'
      AND p.pronargs = 2
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
      AND p.proargtypes[1] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE rv_stddev_pop_impl(random_variable, random_variable) (
      SFUNC     = rv_stat1_impl_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = stddev_pop_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'rv_stddev_samp_impl'
      AND p.pronargs = 2
      AND p.proargtypes[0] = 'provsql.random_variable'::regtype::oid
      AND p.proargtypes[1] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE rv_stddev_samp_impl(random_variable, random_variable) (
      SFUNC     = rv_stat1_impl_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = stddev_samp_rv_ffunc
    );
  END IF;
END $$;

/**
 * @brief Mint the @c PROVSQL_ARITH_PERCENTILE gate: the continuous
 *        percentile (SQL @c percentile_cont) over a group of RV rows.
 *
 * @p pairs is the interleaved wire list @c [ind_1, x_1, ..., ind_n, x_n]
 * (each @p ind_i a 0/1 presence-indicator RV).  The @p fraction is
 * text-encoded in the gate's @c extra and participates in the token UUID
 * (two percentiles of the same group at different fractions are distinct
 * gates).  Per Monte Carlo draw, the sampler collects the values whose
 * indicator draws 1, sorts them, and linearly interpolates at the
 * fraction; a draw with no present row is NaN (undefined world).
 */
CREATE OR REPLACE FUNCTION rv_percentile_make(fraction double precision,
                                              pairs uuid[])
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF fraction IS NULL THEN
    RETURN NULL;
  END IF;
  IF fraction < 0 OR fraction > 1 THEN
    RAISE EXCEPTION
      'percentile_cont: fraction must be between 0 and 1 (got %)', fraction;
  END IF;
  token := public.uuid_generate_v5(
    uuid_ns_provsql(),
    concat('arith', '10', pairs::text, fraction::text));
  PERFORM create_gate(token, 'arith', pairs);
  PERFORM set_infos(token, 10);  -- 10 = PROVSQL_ARITH_PERCENTILE
  PERFORM set_extra(token, fraction::text);
  RETURN random_variable_make(token);
END
$$ LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

/** @brief State transition for the public ordered-set
 *  @c percentile_cont(float8) WITHIN GROUP (ORDER BY random_variable):
 *  append the certain indicator and the row's RV.  Only reachable on
 *  untracked input (a provenance-tracked query is rewritten to
 *  @c rv_percentile_impl before planning), where the sort over
 *  @c random_variable raises the ordering-is-meaningless diagnostic
 *  first -- so in practice this runs only for empty input. */
CREATE OR REPLACE FUNCTION percentile_cont_rv_sfunc(
  state uuid[], x random_variable)
  RETURNS uuid[] AS
$$
  SELECT provsql.rv_stat1_sfunc(state, x);
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

/** @brief Final function for the public ordered-set @c percentile_cont:
 *  receives the direct @p fraction argument after the state. */
CREATE OR REPLACE FUNCTION percentile_cont_rv_ffunc(
  state uuid[], fraction double precision)
  RETURNS random_variable AS
$$
  SELECT CASE
    WHEN state IS NULL OR array_length(state, 1) IS NULL THEN NULL
    ELSE provsql.rv_percentile_make(fraction, state)
  END;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'percentile_cont'
      AND p.pronargs = 2
      AND p.proargtypes[0] = 'pg_catalog.float8'::regtype::oid
      AND p.proargtypes[1] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE percentile_cont(double precision ORDER BY random_variable) (
      SFUNC     = percentile_cont_rv_sfunc,
      STYPE     = uuid[],
      INITCOND  = '{}',
      FINALFUNC = percentile_cont_rv_ffunc
    );
  END IF;
END $$;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_type t
      JOIN pg_namespace n ON n.oid = t.typnamespace
    WHERE n.nspname = 'provsql' AND t.typname = 'rv_percentile_state'
  ) THEN
    CREATE TYPE rv_percentile_state AS (
      fraction double precision,
      tokens uuid[]
    );
  END IF;
END $$;

/** @brief State transition for @c rv_percentile_impl, the planner-hook
 *  rewrite target of a provenance-tracked @c percentile_cont: stashes the
 *  (group-constant) fraction and appends the indicator/value pair. */
CREATE OR REPLACE FUNCTION rv_percentile_impl_sfunc(
  state rv_percentile_state, fraction double precision,
  ind random_variable, x random_variable)
  RETURNS rv_percentile_state AS
$$
  SELECT ROW(
    coalesce((state).fraction, fraction),
    CASE
      WHEN x IS NULL THEN (state).tokens
      ELSE (state).tokens ||
           ARRAY[coalesce((ind)::uuid,
                   (provsql.as_random(1::double precision))::uuid),
                 (x)::uuid]
    END)::provsql.rv_percentile_state;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

/** @brief Final function for @c rv_percentile_impl. */
CREATE OR REPLACE FUNCTION rv_percentile_impl_ffunc(state rv_percentile_state)
  RETURNS random_variable AS
$$
  SELECT CASE
    WHEN state IS NULL OR array_length((state).tokens, 1) IS NULL THEN NULL
    ELSE provsql.rv_percentile_make((state).fraction, (state).tokens)
  END;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'rv_percentile_impl'
      AND p.pronargs = 3
      AND p.proargtypes[0] = 'pg_catalog.float8'::regtype::oid
      AND p.proargtypes[1] = 'provsql.random_variable'::regtype::oid
      AND p.proargtypes[2] = 'provsql.random_variable'::regtype::oid
  ) THEN
    CREATE AGGREGATE rv_percentile_impl(
      double precision, random_variable, random_variable) (
      SFUNC     = rv_percentile_impl_sfunc,
      STYPE     = rv_percentile_state,
      INITCOND  = '(,"{}")',
      FINALFUNC = rv_percentile_impl_ffunc
    );
  END IF;
END $$;

/**
 * @brief Short alias of @c probability_evaluate.
 *
 * Bound to the same C symbol as @c probability_evaluate, so
 * @c probability(token) is exactly @c probability_evaluate(token).
 * Provided to match the concise polymorphic surface of @c expected,
 * @c variance, and @c support: callers are not forced to spell out
 * @c probability_evaluate.
 *
 * @param token provenance token to evaluate
 * @param method knowledge compilation method (NULL for default)
 * @param arguments additional arguments for the method
 */
CREATE OR REPLACE FUNCTION probability(
  token UUID,
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS
  'provsql','probability_evaluate' LANGUAGE C STABLE;

/**
 * @brief Probability of a Boolean event over random variables.
 *
 * The @c (boolean) overload of @c probability lets a query ask for the
 * probability of an event with the natural infix grammar, e.g.
 * @c probability(x @c > @c y @c AND @c x @c < @c z).  When the argument
 * carries a probabilistic (random_variable / aggregate) comparison, the
 * planner hook intercepts the call and rewrites it into
 * @c probability_evaluate over the argument's event token (a @c gate_cmp /
 * Boolean combination); the body below is then never reached.
 *
 * When the argument is a purely deterministic Boolean (no probabilistic
 * comparison) the hook leaves the call alone and the body runs, so the
 * probability of a definite event is simply @c 1 when it holds and @c 0 when
 * it does not (@c NULL propagates).  This makes @c probability total over
 * Booleans -- @c probability(1 @c > @c 0) is @c 1, @c probability(region @c =
 * @c 'north') is a per-row @c 0/1 -- and it works even with
 * @c provsql.active off.  @c NOT strict so a default-NULL @c method does not
 * short-circuit the cast.
 *
 * The predicate surface deliberately lives only on the short @c probability
 * name, not on @c probability_evaluate: a Boolean overload of the latter
 * would make @c probability_evaluate('<uuid-as-text>') ambiguous (an unknown
 * literal matches both the @c uuid and the @c boolean overload), breaking
 * existing string-literal callers.  @c probability is new, so it carries the
 * predicate overload without that hazard.
 */
CREATE OR REPLACE FUNCTION probability(
  predicate boolean,
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS
$$
  SELECT predicate::integer::double precision;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

/** @brief Exact E[AVG^k | COUNT >= 1] over independent rows (the joint
 *  (sum, count) fold); NULL when the shape is out of scope (shared
 *  leaves, compound contributors), signalling @c agg_raw_moment's avg
 *  arm to fall back to the Monte-Carlo scalar path. */
CREATE OR REPLACE FUNCTION agg_avg_moment_exact(token uuid, k integer)
  RETURNS double precision
  AS 'provsql','agg_avg_moment_exact' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Boolean event "this aggregate-carrying gate's value is defined
 *        (non-NULL) in the world".
 *
 * Backs the conditional-on-defined convention of the aggregate moment
 * readouts: @c sum / @c count (and constants) have a value in every
 * world -- the empty group is the real value @c 0 -- so their defined
 * event is @c gate_one(); @c min / @c max / @c avg (and any other
 * aggregate) are @c NULL on an empty group, so their defined event is
 * "some contributing row is present", the OR of the semimod children's
 * row tokens; a @c case gate's value is defined iff its first-match
 * selected branch's value is (the same region walk as the moment
 * evaluator, conjoined per branch).  Anything else (an @c arith
 * composite, whose agg_token running value is total) counts as always
 * defined.
 */
CREATE OR REPLACE FUNCTION agg_defined_event(token uuid)
  RETURNS uuid AS $$
DECLARE
  gt provenance_gate := get_gate_type(token);
  fname varchar;
  toks uuid[];
  wires uuid[];
  nw integer;
  m integer;
  i integer;
  running_neg uuid := gate_one();
  parts uuid[] := '{}';
BEGIN
  IF gt = 'agg' THEN
    SELECT proname INTO fname
      FROM pg_proc WHERE oid = (get_infos(token)).info1;
    IF fname IN ('sum', 'count') THEN
      RETURN gate_one();
    END IF;
    SELECT array_agg((get_children(c))[1]) INTO toks
      FROM unnest(get_children(token)) AS c;
    IF toks IS NULL THEN
      RETURN gate_zero();   -- structurally empty aggregate: never defined
    END IF;
    RETURN provenance_plus(toks);
  ELSIF gt = 'case' THEN
    wires := get_children(token);
    nw := array_length(wires, 1);
    m := (nw - 1) / 2;
    FOR i IN 1..m LOOP
      parts := parts || provenance_times(
        running_neg, wires[2 * i - 1],
        agg_defined_event(wires[2 * i]));
      running_neg := provenance_times(running_neg,
                                      provenance_not(wires[2 * i - 1]));
    END LOOP;
    parts := parts || provenance_times(running_neg,
                                       agg_defined_event(wires[nw]));
    RETURN provenance_plus(parts);
  END IF;
  -- value / arith / anything else: a value exists in every world.
  RETURN gate_one();
END
$$ LANGUAGE plpgsql STABLE STRICT PARALLEL SAFE
  SET search_path=provsql,pg_temp,public SECURITY DEFINER;

/**
 * @brief Compute the raw moment E[X^k | prov] of an agg_token aggregate
 *
 * Sister of @c expected() for the agg_token side of the polymorphic
 * @c moment / @c variance / @c central_moment dispatch.  Supports the
 * same aggregation functions as @c expected: SUM (which COUNT
 * normalises to at the gate level via @c Aggregation.cpp:322), MIN,
 * MAX, and AVG (exact over independent / laminar rows via the joint
 * (sum, count) distribution, Monte-Carlo scalar fallback otherwise).
 * MIN / MAX / AVG are NULL on an empty group, so their moments are
 * CONDITIONAL on the aggregate being defined -- NULL only when it never
 * is; SUM / COUNT treat the empty world as the real value 0.
 *
 * Strategy:
 * - <b>SUM</b>: with X = Σᵢ Iᵢ·vᵢ (Iᵢ the per-row inclusion indicator,
 *   vᵢ the row's value), expanding X^k and taking expectation gives
 *   @f$E[X^k] = \sum_{(i_1,\ldots,i_k) \in \{1..n\}^k} v_{i_1}\cdots v_{i_k}
 *               \cdot P(\bigwedge_{i \in \text{distinct}(i_1..i_k)} I_i)@f$.
 *   We enumerate the @f$n^k@f$ tuples, conjoin the distinct inclusion
 *   tokens (and @p prov when conditioning), and evaluate the
 *   probability via @c probability_evaluate.
 * - <b>MIN / MAX</b>: replace @c v with @c v^k in the rank-based
 *   enumeration that @c expected already uses; @c MAX is handled by
 *   sign-flipping per the existing trick (negate vs.  rerank), with
 *   the outer multiplier becoming @f$(-1)^k@f$ instead of just @f$-1@f$.
 *
 * Cost: SUM is @f$O(n^k)@f$ probability evaluations -- tractable for
 * small @p k or small @p n; for larger sizes, prefer reaching for the
 * sampler.  MIN / MAX stay linear in @p n.
 */
CREATE OR REPLACE FUNCTION agg_raw_moment(
  token agg_token,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  aggregation_function VARCHAR;
  child_pairs uuid[];
  pair_children uuid[];
  n integer;
  i integer;
  j integer;
  vals float8[];
  toks uuid[];
  total float8;
  total_probability float8;
  tup integer[];
  d integer;
  prod_v float8;
  distinct_tok uuid[];
  conj_token uuid;
  prob float8;
  sign_max float8;
BEGIN
  IF token IS NULL OR k IS NULL THEN
    RETURN NULL;
  END IF;
  IF k < 0 THEN
    RAISE EXCEPTION 'agg_raw_moment(): k must be non-negative (got %)', k;
  END IF;

  -- Aggregate-carrier CASE (a gate_case over aggregate branches): a first-match
  -- guarded selection.  The moment is CONDITIONAL on the CASE's value being
  -- defined (NULL only when it never is, mirroring the MIN/MAX convention):
  --   E[pick^k | defined ∧ prov]
  --     = Σ_i P(region_i ∧ def_i) · E[value_i^k | region_i ∧ def_i]
  --       / Σ_i P(region_i ∧ def_i),
  -- where region_i = (¬g_1 ∧ … ∧ ¬g_{i-1}) ∧ g_i ∧ prov is the world set that
  -- selects branch i (the default's region is "all guards false") and def_i is
  -- the branch's defined event (agg_defined_event: gate_one for sum / count /
  -- constants, "some row present" for min / max / avg, recursive for a nested
  -- CASE).  Both factors are exact: probability() over the region ∧ def event,
  -- and the conditional aggregate moment (a recursive agg_raw_moment on the
  -- branch aggregate, which conditions on its own definedness within the
  -- region, so the two factors weigh the same worlds).  The regions are
  -- mutually exclusive, so the terms sum with no inclusion-exclusion, and
  -- correlation between a guard and its branch (shared input tuples) is
  -- carried by the conditioning, exactly as HAVING carries it.  When every
  -- branch is defined everywhere, the defined mass equals P(prov) and the
  -- formula reduces to the plain region-weighted sum.
  IF get_gate_type(token) = 'case' THEN
    IF k = 0 THEN
      RETURN 1;
    END IF;
    DECLARE
      wires uuid[] := get_children(token);
      nw integer := array_length(get_children(token), 1);
      m integer := (array_length(get_children(token), 1) - 1) / 2;
      running_neg uuid := gate_one();
      region_full uuid;
      prov_p float8;
      p float8;
      total float8 := 0;
      def_mass float8 := 0;
      ci integer;
      vuid uuid;
      bm float8;
    BEGIN
      prov_p := probability(prov);
      IF prov_p IS NULL OR prov_p <= 0 THEN
        RETURN NULL;   -- impossible conditioning event
      END IF;
      -- Branches 1..m are the guarded WHENs; branch m+1 is the ELSE default,
      -- whose region is "all guards false".
      FOR ci IN 1 .. m + 1 LOOP
        IF ci <= m THEN
          region_full := provenance_times(running_neg, wires[2 * ci - 1], prov);
          vuid := wires[2 * ci];
          running_neg :=
            provenance_times(running_neg, provenance_not(wires[2 * ci - 1]));
        ELSE
          region_full := provenance_times(running_neg, prov);
          vuid := wires[nw];
        END IF;
        p := probability(provenance_times(region_full,
                                          agg_defined_event(vuid)));
        IF p > 0 THEN
          -- E[value_i^k | region_i ∧ def_i]: a constant branch is a Dirac
          -- (c^k, exact); a single aggregate or nested CASE is exact via
          -- agg_raw_moment (whose MIN/MAX/CASE arms condition on their own
          -- definedness within the region); an arithmetic / composite branch
          -- takes the Monte-Carlo scalar path (which composes with the
          -- aggregate leaves).
          IF get_gate_type(vuid) = 'value' THEN
            bm := power(CAST(get_extra(vuid) AS float8), k);
          ELSIF get_gate_type(vuid) IN ('agg', 'case') THEN
            bm := agg_raw_moment(agg_token_make(vuid, 0), k, region_full,
                                 method, arguments);
          ELSE
            bm := rv_moment(vuid, k, false, region_full);
          END IF;
          total := total + p * bm;
          def_mass := def_mass + p;
        END IF;
      END LOOP;
      IF def_mass <= epsilon() THEN
        RETURN NULL;   -- the CASE's value is never defined under prov
      END IF;
      RETURN total / def_mass;
    END;
  END IF;

  IF get_gate_type(token) <> 'agg' THEN
    IF get_gate_type(token) IN ('arith', 'conditioned') THEN
      RAISE EXCEPTION 'expected / variance / moment over an arithmetic '
        'combination of aggregates (e.g. SUM(x) + SUM(y) or SUM(x) + 5), or a '
        'conditioning of one, is not yet supported: a moment can be taken only '
        'over a single aggregate (SUM / COUNT / MIN / MAX), optionally '
        'conditioned (SUM(x) | C)'
        USING HINT = 'Take the moment of each aggregate separately, or condition '
          'the bare aggregate.';
    ELSE
      RAISE EXCEPTION USING MESSAGE='Wrong gate type for agg_raw_moment computation';
    END IF;
  END IF;
  IF k = 0 THEN
    RETURN 1;
  END IF;

  SELECT pp.proname::varchar FROM pg_proc pp
    WHERE oid=(get_infos(token)).info1
    INTO aggregation_function;

  child_pairs := get_children(token);
  n := COALESCE(array_length(child_pairs, 1), 0);

  IF aggregation_function = 'sum' OR aggregation_function = 'count' THEN
    -- count(col) keeps the COUNT identity at the gate level but its value is a
    -- SUM of per-row 0/1 indicators, so its moments are computed exactly like
    -- SUM (and its empty group is the real value 0, like SUM).  count(*)
    -- arrives here as 'sum' (it normalises to F_SUM_INT4); count(col) as 'count'.
    -- Trivial empty aggregation: SUM = 0, so SUM^k = 0 for k >= 1.
    -- Note: agg_token semantics treat the "no row included" world as
    -- SUM = 0, so this stays consistent with k = 1 (= expected()).
    IF n = 0 THEN
      RETURN 0;
    END IF;

    -- Extract per-child token + value arrays.
    vals := ARRAY[]::float8[];
    toks := ARRAY[]::uuid[];
    FOR i IN 1..n LOOP
      pair_children := get_children(child_pairs[i]);
      toks := toks || pair_children[1];
      vals := vals || CAST(get_extra(pair_children[2]) AS float8);
    END LOOP;

    -- Enumerate all k-tuples (i_1, ..., i_k) in {1..n}^k.  tup is the
    -- current tuple; we step through them in lexicographic order.
    total := 0;
    tup := array_fill(1, ARRAY[k]);
    LOOP
      prod_v := 1;
      FOR j IN 1..k LOOP
        prod_v := prod_v * vals[tup[j]];
      END LOOP;

      SELECT array_agg(DISTINCT toks[idx]) INTO distinct_tok
        FROM unnest(tup) AS idx;

      IF prov <> gate_one() THEN
        distinct_tok := distinct_tok || prov;
      END IF;
      conj_token := provenance_times(VARIADIC distinct_tok);
      prob := probability_evaluate(conj_token, method, arguments);

      total := total + prod_v * prob;

      d := k;
      WHILE d >= 1 AND tup[d] = n LOOP
        tup[d] := 1;
        d := d - 1;
      END LOOP;
      EXIT WHEN d = 0;
      tup[d] := tup[d] + 1;
    END LOOP;
  ELSIF aggregation_function = 'min' OR aggregation_function = 'max' THEN
    -- Rank enumeration: per distinct value v, P(MIN = v) is the
    -- probability that some t_i with v_i=v is true and all t_j with
    -- smaller v are false.  For MAX we negate values so the same
    -- "smaller-than" rank logic computes MIN-of-negated, then flip.
    -- The outer multiplier picks up the right sign for the k-th moment
    -- of MAX: E[MAX^k] = (-1)^k * E[MIN(-v)^k], so sign_max = (-1)^k.
    sign_max := CASE
                  WHEN aggregation_function = 'max'
                  THEN power(-1::float8, k)
                  ELSE 1
                END;

    -- MIN/MAX over the empty input world are NULL (no elements), not ±Infinity:
    -- SQL returns one row with a NULL value.  The moment is therefore CONDITIONAL
    -- on the aggregate being defined (non-empty) -- the empty world is excluded
    -- and the result renormalised by P(prov AND non-empty).  (count, whose empty
    -- value 0 is a real value, keeps the empty world; sum keeps it too, as 0.)
    IF n = 0 THEN
      RETURN NULL;  -- structurally empty: MIN/MAX undefined
    END IF;

    -- Numerator E[MIN^k . 1{prov AND non-empty}] (the rank sum naturally omits
    -- the empty world, since every term requires a present token).
    WITH tok_value AS (
      SELECT (get_children(c))[1] AS tok,
             (CASE WHEN aggregation_function='max' THEN -1 ELSE 1 END)
               * CAST(get_extra((get_children(c))[2]) AS DOUBLE PRECISION) AS v
      FROM UNNEST(child_pairs) AS c
    ) SELECT sign_max * COALESCE(SUM(p * power(v, k)), 0) FROM (
        SELECT t1.v AS v,
          probability_evaluate(
            CASE WHEN prov = gate_one()
                 THEN provenance_monus(provenance_plus(ARRAY_AGG(t1.tok)),
                                       provenance_plus(ARRAY_AGG(t2.tok)))
                 ELSE provenance_times(prov,
                        provenance_monus(provenance_plus(ARRAY_AGG(t1.tok)),
                                         provenance_plus(ARRAY_AGG(t2.tok)))) END,
            method, arguments) AS p
        FROM tok_value t1 LEFT OUTER JOIN tok_value t2 ON t1.v > t2.v
        GROUP BY t1.v) tmp
      INTO total;

    -- Denominator P(prov AND non-empty) = P(prov (x) (+) tokens).
    SELECT probability_evaluate(
             CASE WHEN prov = gate_one()
                  THEN provenance_plus(ARRAY_AGG(tok))
                  ELSE provenance_times(prov, provenance_plus(ARRAY_AGG(tok))) END,
             method, arguments)
      FROM (SELECT (get_children(c))[1] AS tok FROM UNNEST(child_pairs) AS c) s
      INTO total_probability;

    IF total_probability <= epsilon() THEN
      RETURN NULL;  -- never defined under prov: MIN/MAX undefined
    END IF;
    RETURN total / total_probability;  -- already conditional; skip generic norm
  ELSIF aggregation_function = 'avg' THEN
    -- AVG = SUM/COUNT is a ratio of two correlated world-dependent
    -- quantities, so the k-tuple expansion above does not apply.  Like
    -- MIN/MAX, AVG over the empty world is NULL, so its moment conditions
    -- on the aggregate being defined (COUNT >= 1), NULL when it never is.
    -- Two routes:
    --  * EXACT (independent rows, unconditional): the joint (sum, count)
    --    PMF folded in C by agg_avg_moment_exact --
    --    E[AVG^k | COUNT>=1] = Σ_{(s,c), c>=1} (s/c)^k pmf(s,c) / P(c>=1).
    --  * Monte-Carlo scalar fallback otherwise (an outer conditioning
    --    event, shared leaves, compound contributors): rv_moment samples
    --    the agg gate per world; its NaN-skip on empty draws implements
    --    the same conditional-on-defined convention, at the
    --    provsql.rv_mc_samples budget (0 raises, per convention).
    IF n = 0 THEN
      RETURN NULL;  -- structurally empty: AVG undefined
    END IF;
    IF prov = gate_one() THEN
      total := agg_avg_moment_exact((token)::uuid, k);
      IF total IS NOT NULL THEN
        RETURN total;
      END IF;
    END IF;
    RETURN rv_moment((token)::uuid, k, false, prov);
  ELSE
    RAISE EXCEPTION USING MESSAGE=
      'Cannot compute moment for aggregation function ' || aggregation_function;
  END IF;

  -- Conditional normalisation: E[X^k · 1_A] / P(A) = E[X^k | A].
  IF prov <> gate_one()
     AND total <> 0
     AND total <> 'Infinity'::float8
     AND total <> '-Infinity'::float8 THEN
    total := total / probability_evaluate(prov, method, arguments);
  END IF;

  RETURN total;
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

/**
 * @brief Internal: rv-side quantile computation.
 *
 * C entry point behind the polymorphic @c quantile dispatcher.
 * Closed-form inverse CDF where the family has one (Normal via
 * Beasley-Springer-Moro polished by Newton steps, Uniform and
 * Exponential by algebraic inversion), generic monotone-CDF bisection
 * otherwise (Erlang, Gamma), exact generalised inverse for categorical
 * mixtures, and the empirical Monte Carlo quantile for compound scalar
 * circuits.  A non-trivial @p prov conditions (truncates) the
 * distribution first, in closed form when the event reduces to an
 * interval on a bare @c gate_rv.
 */
CREATE OR REPLACE FUNCTION rv_quantile(
  token uuid, p double precision,
  prov uuid DEFAULT gate_one())
  RETURNS double precision
  AS 'provsql','rv_quantile' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Compute the p-quantile (inverse CDF) of a probabilistic scalar
 *
 * @f$F^{-1}(p) = \min\{x : P(X \le x) \ge p\}@f$ for @f$p \in [0,1]@f$:
 * medians (@c p = 0.5), percentiles, Value-at-Risk, and credible
 * intervals.  @c p = 0 / @c p = 1 return the (possibly infinite)
 * support edges.  Polymorphic dispatcher mirroring @c expected /
 * @c moment: @c random_variable routes through @c rv_quantile
 * (analytical inverse CDF / MC), plain numerics are their own quantile
 * (a Dirac's inverse CDF is constant), and the optional @p prov
 * argument conditions on a provenance event, e.g.
 * <tt>quantile(x | (x > 0), 0.5)</tt> for the median of a truncated
 * distribution.
 */
CREATE OR REPLACE FUNCTION quantile(
  input ANYELEMENT,
  p double precision,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
BEGIN
  IF p IS NULL THEN
    RETURN NULL;
  END IF;
  IF p <> p OR p < 0 OR p > 1 THEN
    RAISE EXCEPTION 'quantile: p must be in [0, 1] (got %)', p;
  END IF;

  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL THEN
      RETURN NULL;
    END IF;
    -- See variance(): rv_quantile handles the conditional/unconditional
    -- dispatch internally based on the resolved prov gate type.
    RETURN provsql.rv_quantile(
      rv_conditioned_target((input::random_variable)::uuid), p,
      rv_conditioned_prov((input::random_variable)::uuid, prov));
  END IF;

  IF pg_typeof(input) IN ('smallint'::regtype, 'integer'::regtype,
                          'bigint'::regtype, 'numeric'::regtype,
                          'real'::regtype, 'double precision'::regtype) THEN
    -- A deterministic scalar is a Dirac: every quantile is the value.
    RETURN input::double precision;
  END IF;

  RAISE EXCEPTION 'quantile() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

/**
 * @brief Covariance Cov(X, Y) = E[XY] − E[X]·E[Y] of two random variables.
 *
 * The bivariate readout complementing the univariate moment surface
 * (@ref expected / @ref variance / @ref moment / @ref central_moment).  It
 * reduces entirely to the existing scalar machinery: @ref expected on the
 * @c gate_arith @c TIMES product @c X*Y and on each factor.  The
 * @c Expectation evaluator's @c FootprintCache structural-independence path
 * makes it exact where it can -- disjoint @c gate_rv footprints give
 * @c E[XY] = E[X]·E[Y] and hence an exact @c 0 -- and correlation-aware when
 * @p x and @p y share leaves, with the whole-circuit Monte-Carlo net
 * inherited for free.
 *
 * @param prov optional conditioning event (a provenance @c uuid); the
 *   default @c gate_one() is the unconditional covariance.  Conditioning is
 *   applied consistently to the product and to each factor, giving
 *   @c Cov(X, Y | prov) = E[XY|prov] − E[X|prov]·E[Y|prov].
 */
CREATE OR REPLACE FUNCTION covariance(
  x random_variable, y random_variable, prov uuid DEFAULT gate_one())
  RETURNS double precision AS $$
  SELECT provsql.expected(x * y, prov)
       - provsql.expected(x, prov) * provsql.expected(y, prov);
$$ LANGUAGE sql PARALLEL SAFE STABLE SET search_path=provsql SECURITY DEFINER;

/**
 * @brief Standard deviation σ(X) = √Var(X) of a random variable.
 *
 * A thin numeric readout over @ref variance: the square root is taken on
 * the scalar @c double result, so no RV-level @c sqrt is involved and this
 * carries no dependency on RV function application (@c pow / @c sqrt).
 * @c NULL propagates from a @c NULL input; the order-2 central moment is
 * non-negative by construction, so the root is always real.
 *
 * @param prov optional conditioning event; default @c gate_one()
 *   (unconditional).
 */
CREATE OR REPLACE FUNCTION stddev(
  x random_variable, prov uuid DEFAULT gate_one())
  RETURNS double precision AS $$
  SELECT sqrt(provsql.variance(x, prov));
$$ LANGUAGE sql PARALLEL SAFE STABLE SET search_path=provsql SECURITY DEFINER;

/**
 * @brief Pearson correlation ρ(X, Y) = Cov(X, Y) / (σ(X)·σ(Y)).
 *
 * A scalar division of @ref covariance by the two @ref stddev readouts.
 * Returns @c NULL when either standard deviation is @c 0 (a degenerate /
 * constant variable, for which correlation is undefined) rather than
 * raising a division-by-zero.
 *
 * @param prov optional conditioning event; default @c gate_one()
 *   (unconditional).
 */
CREATE OR REPLACE FUNCTION correlation(
  x random_variable, y random_variable, prov uuid DEFAULT gate_one())
  RETURNS double precision AS $$
  SELECT provsql.covariance(x, y, prov)
       / NULLIF(provsql.stddev(x, prov) * provsql.stddev(y, prov), 0);
$$ LANGUAGE sql PARALLEL SAFE STABLE SET search_path=provsql SECURITY DEFINER;

/** @brief C entry point behind @ref entropy (uuid-level binding). */
CREATE OR REPLACE FUNCTION rv_entropy(token uuid, prov uuid)
  RETURNS double precision
  AS 'provsql','rv_entropy' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Entropy H(X) of a random variable, in nats.
 *
 * Shannon entropy for a discrete distribution (a categorical / discrete
 * count / constant -- a point mass has entropy @c 0), differential
 * entropy for a continuous one (quadrature of @c -f ln f over the
 * family's integration range; also exact through independent-arm
 * Bernoulli mixture trees such as @ref gmm's).  Shapes with no
 * closed density (arithmetic composites) and the conditional form fall
 * back to a Monte Carlo histogram plug-in estimate at the
 * @c provsql.rv_mc_samples budget.
 *
 * @param prov optional conditioning event; default @c gate_one()
 *   (unconditional).
 */
CREATE OR REPLACE FUNCTION entropy(
  x random_variable, prov uuid DEFAULT gate_one())
  RETURNS double precision AS $$
  SELECT provsql.rv_entropy((x)::uuid, prov);
$$ LANGUAGE sql PARALLEL SAFE STABLE SET search_path=provsql SECURITY DEFINER;

/** @brief C entry point behind @ref kl (uuid-level binding). */
CREATE OR REPLACE FUNCTION rv_kl(p uuid, q uuid)
  RETURNS double precision
  AS 'provsql','rv_kl' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Kullback-Leibler divergence KL(P || Q), in nats.
 *
 * Exact: the defining sum for two discrete distributions (matching
 * outcomes by value) and the defining integral (quadrature over P's
 * integration window) for two continuous ones, including
 * independent-arm mixture trees.  Returns @c Infinity when P is not
 * absolutely continuous with respect to Q -- an outcome of P that Q
 * gives zero mass, mismatched kinds (discrete vs continuous), or a
 * region of P's support where Q's density (under)flows to zero.  Both
 * arguments must resolve to closed-form densities; arithmetic
 * composites and conditioned variables raise.
 */
CREATE OR REPLACE FUNCTION kl(p random_variable, q random_variable)
  RETURNS double precision AS $$
  SELECT provsql.rv_kl((p)::uuid, (q)::uuid);
$$ LANGUAGE sql PARALLEL SAFE STABLE SET search_path=provsql SECURITY DEFINER;

/** @brief C entry point behind @ref mutual_information (uuid-level
 *  binding). */
CREATE OR REPLACE FUNCTION rv_mutual_information(x uuid, y uuid)
  RETURNS double precision
  AS 'provsql','rv_mutual_information' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/**
 * @brief Mutual information I(X; Y), in nats.
 *
 * Exactly @c 0 for structurally independent variables (disjoint
 * stochastic-leaf footprints, the same test the moment evaluators use);
 * @c H(X) for a discrete variable paired with itself and @c Infinity
 * for a continuous one (I(X;X) diverges).  A genuinely correlated pair
 * (shared leaves) is estimated by a 2-D histogram plug-in over coupled
 * joint Monte Carlo draws -- both roots evaluated against the same
 * per-iteration cache, so shared leaves keep their joint law -- at the
 * @c provsql.rv_mc_samples budget.
 */
CREATE OR REPLACE FUNCTION mutual_information(
  x random_variable, y random_variable)
  RETURNS double precision AS $$
  SELECT provsql.rv_mutual_information((x)::uuid, (y)::uuid);
$$ LANGUAGE sql PARALLEL SAFE STABLE SET search_path=provsql SECURITY DEFINER;

-- ---------------------------------------------------------------------
-- NULL-semantics surface: the deprecated aggregation_evaluate driver is
-- retired, the core combinators gain three-valued-logic NULL handling,
-- avg gains its value-aware presence indicator, and the explicit
-- zero-filtering predicates are added.  Bodies are transcribed verbatim
-- from provsql.common.sql so md5(prosrc) matches a fresh install.
-- ---------------------------------------------------------------------

-- Retire both aggregation_evaluate overloads (a NULL-token crash vector).
-- The C symbol stays in provsql.so as an always-NULL stub for the 1.0.0
-- fixture; only the SQL surface is dropped.
DROP FUNCTION IF EXISTS aggregation_evaluate(uuid, regclass, regproc, regproc, regproc, anyelement, regproc, regproc, regproc, regproc);
DROP FUNCTION IF EXISTS aggregation_evaluate(uuid, regclass, regproc, regproc, regproc, anyelement, regtype, regproc, regproc, regproc, regproc);

CREATE OR REPLACE FUNCTION provenance_times(VARIADIC tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  times_token uuid;
  filtered_tokens uuid[];
  canonical uuid;
BEGIN
  -- A NULL element reads as the ⊗-neutral 1: it is the token slot of an
  -- untracked source (a join against an untracked table), which is
  -- certain.  Contrast provenance_plus / provenance_monus, where NULL
  -- reads as the ⊕- / ⊖-right-neutral 0: each combinator maps NULL to
  -- its own neutral element.  Nothing may therefore hand a NULL to ⊗
  -- meaning "false"; a comparison with a NULL operand goes through
  -- provenance_cmp, which returns gate_zero for it.
  SELECT array_agg(t) FROM unnest(tokens) t WHERE t IS NOT NULL AND t <> gate_one() INTO filtered_tokens;

  -- Dispatch on the FILTERED count: a single survivor short-circuits
  -- to that token directly (no useless single-child times gate); zero
  -- survivors collapse to the identity. Using array_length(tokens, 1)
  -- here would miss the [one, cmp] → [cmp] case, leaving the cmp wrapped
  -- in a one-child times when its only sibling was gate_one().
  CASE coalesce(array_length(filtered_tokens, 1), 0)
    WHEN 0 THEN
      times_token:=gate_one();
    WHEN 1 THEN
      times_token:=filtered_tokens[1];
    ELSE
      -- Computed separately from the filtering aggregate above: an
      -- ORDER BY aggregate there would make the planner feed *both*
      -- aggregates sorted input, scrambling the stored children order.
      SELECT uuid_generate_v5(uuid_ns_provsql(),
                              concat('times-canonical', array_agg(t ORDER BY t)))
      FROM unnest(filtered_tokens) t
      INTO canonical;
      IF get_gate_type(canonical) = 'times' THEN
        -- A deliberate pre-creation at the canonical address: same
        -- children, same product.
        times_token := canonical;
      ELSE
        times_token := uuid_generate_v5(uuid_ns_provsql(),concat('times',filtered_tokens));

        PERFORM create_gate(times_token, 'times', ARRAY_AGG(t)) FROM UNNEST(filtered_tokens) AS t WHERE t IS NOT NULL;
      END IF;
  END CASE;

  RETURN times_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE;

CREATE OR REPLACE FUNCTION provenance_plus(tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  c INTEGER;
  plus_token uuid;
  filtered_tokens uuid[];
  canonical uuid;
BEGIN
  -- A NULL element reads as the ⊕-neutral 0: it stands for a row absent
  -- from the disjunction (a null-padded antijoin row whose token array
  -- slot is NULL), not for an untracked source.  Contrast provenance_times,
  -- where NULL reads as the ⊗-neutral 1 (untracked source): each
  -- combinator maps NULL to its own neutral element.
  SELECT array_agg(t) FROM unnest(tokens) t
  WHERE t IS NOT NULL AND t <> gate_zero()
  INTO filtered_tokens;

  c:=array_length(filtered_tokens, 1);

  IF c = 0 THEN
    plus_token := gate_zero();
  ELSIF c = 1 THEN
    plus_token := filtered_tokens[1];
  ELSE
    -- Computed separately from the filtering aggregate above: an ORDER
    -- BY aggregate there would make the planner feed *both* aggregates
    -- sorted input, scrambling the stored (aggregation-order) children.
    SELECT uuid_generate_v5(uuid_ns_provsql(),
                            concat('plus-canonical', array_agg(t ORDER BY t)))
    FROM unnest(filtered_tokens) t
    INTO canonical;
    IF get_gate_type(canonical) = 'plus' THEN
      -- A deliberate pre-creation at the canonical address: same
      -- children, same sum.
      plus_token := canonical;
    ELSE
      plus_token := uuid_generate_v5(
        uuid_ns_provsql(),
        concat('plus', filtered_tokens));

      PERFORM create_gate(plus_token, 'plus', filtered_tokens);
    END IF;
  END IF;

  RETURN plus_token;
END
$$ LANGUAGE plpgsql STRICT SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_monus(token1 UUID, token2 UUID)
  RETURNS UUID AS
$$
DECLARE
  monus_token uuid;
BEGIN
  IF token1 IS NULL THEN
    RAISE EXCEPTION USING MESSAGE='provenance_monus is called with first argument NULL';
  END IF;

  IF token2 IS NULL THEN
    -- The ⊖-right-neutral 0: a NULL second argument is the no-match case
    -- of the difference operator's LEFT OUTER JOIN (nothing to subtract),
    -- so X ⊖ NULL = X ⊖ 0 = X.  Note this is NOT the NULL ≡ 1 reading of
    -- provenance_times; each combinator maps NULL to its own neutral.
    RETURN token1;
  END IF;

  IF token1 = token2 THEN
    -- X-X=0
    monus_token:=gate_zero();
  ELSIF token1 = gate_zero() THEN
    -- 0-X=0
    monus_token:=gate_zero();
  ELSIF token2 = gate_zero() THEN
    -- X-0=X
    monus_token:=token1;
  ELSE
    monus_token:=uuid_generate_v5(uuid_ns_provsql(),concat('monus',token1,token2));
    PERFORM create_gate(monus_token, 'monus', ARRAY[token1::uuid, token2::uuid]);
  END IF;

  RETURN monus_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_cmp(
  left_token  UUID,
  comparison_op OID,
  right_token UUID
)
RETURNS UUID AS
$$
DECLARE
  cmp_token UUID;
BEGIN
  -- A comparison with a NULL operand (a NULL random_variable cell, or an
  -- aggregate that is NULL on the instance) is unknown under SQL's 3VL in
  -- every possible world: the row is annotated zero.  The function must
  -- not be STRICT: a NULL result would read as the neutral token
  -- (provenance_times drops it), silently turning "unknown" into
  -- "certainly true".
  IF left_token IS NULL OR right_token IS NULL OR comparison_op IS NULL THEN
    RETURN gate_zero();
  END IF;
  -- deterministic v5 namespace id
  cmp_token := public.uuid_generate_v5(
    uuid_ns_provsql(),
    concat('cmp', left_token::text, comparison_op::text, right_token::text)
  );
  -- wire it up in the circuit
  PERFORM create_gate(cmp_token, 'cmp', ARRAY[left_token, right_token]);
  PERFORM set_infos(cmp_token, comparison_op::integer);
  RETURN cmp_token;
END
$$ LANGUAGE plpgsql
  SET search_path=provsql,pg_temp,public
  SECURITY DEFINER
  IMMUTABLE
  PARALLEL SAFE;

CREATE OR REPLACE FUNCTION provenance_delta
  (token UUID)
  RETURNS UUID AS
$$
DECLARE
  delta_token uuid;
BEGIN
  -- NULL token ≡ 1 (untracked source), and δ(1) = 1.  Tested first: the
  -- equality comparisons below are not NULL-safe.
  IF token IS NULL THEN
    return gate_one();
  END IF;

  IF token = gate_zero() OR token = gate_one() THEN
    return token;
  END IF;

  delta_token:=uuid_generate_v5(uuid_ns_provsql(),concat('delta',token));

  PERFORM create_gate(delta_token,'delta',ARRAY[token::uuid]);

  RETURN delta_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_aggregate_indicator(prov uuid, rv random_variable)
  RETURNS random_variable AS
$$
  SELECT CASE WHEN rv IS NULL THEN NULL
              ELSE provsql.rv_aggregate_indicator(prov) END;
$$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION true_nonzero(token uuid)
  RETURNS boolean AS
  'provsql', 'true_nonzero' LANGUAGE C PARALLEL SAFE STABLE;

CREATE OR REPLACE FUNCTION nonzero(token uuid,
                        semiring text DEFAULT NULL,
                        mapping regclass DEFAULT NULL)
  RETURNS boolean AS
$$
BEGIN
  IF token IS NULL THEN
    RETURN true;
  END IF;
  IF semiring IS NULL THEN
    RETURN provsql.true_nonzero(token);
  ELSIF semiring = 'boolean' THEN
    RETURN provsql.provenance_evaluate_compiled(token, mapping, 'boolean', TRUE);
  ELSIF semiring = 'counting' THEN
    RETURN provsql.provenance_evaluate_compiled(token, mapping, 'counting', 1) <> 0;
  ELSE
    RAISE EXCEPTION 'nonzero: unsupported semiring "%" (supported: boolean, counting; NULL for the universal zero test)', semiring;
  END IF;
END
$$ LANGUAGE plpgsql PARALLEL SAFE STABLE;

CREATE OR REPLACE FUNCTION present(token uuid)
  RETURNS boolean AS
$$
  SELECT provsql.nonzero(token, 'boolean');
$$ LANGUAGE sql PARALLEL SAFE STABLE;

-- A backend warmed under 1.10.0 caches InvalidOid for the new 'case' enum
-- value; force a fresh lookup on the next get_constants() call.

-- ----------------------------------------------------------------------
-- Latent random variables + likelihood-weighting posterior inference.
--
-- This surface (distribution constructors taking random_variable
-- parameters, rv_parametric1/2, the evidence / observe / given
-- conditioning constructors and the and_agg fold, shapley_observe, the
-- agg_token -> random_variable bridge, and the collapsed-moment / variance
-- readouts) was added to the base SQL after 1.10.0 but had not been
-- replicated here.  Functions use CREATE OR REPLACE (idempotent); the
-- non-replaceable objects (the 'observe' enum value near the top, the
-- agg_token -> random_variable cast, the and_agg aggregate, and the unary
-- | given operators) are each guarded so the script stays idempotent and
-- PostgreSQL 10/11-safe (no CREATE OR REPLACE AGGREGATE, no bare CREATE).
-- ----------------------------------------------------------------------

-- given_predicate(boolean) was folded into the given(boolean) overload;
-- drop the stale prefix operator (it points at given_predicate) and the
-- function so the upgraded catalog matches a fresh install.  The
-- given(boolean) form and its | operator are (re)created further down.
DROP OPERATOR IF EXISTS | (NONE, boolean);
DROP FUNCTION IF EXISTS given_predicate(boolean);


CREATE OR REPLACE FUNCTION provenance_times(VARIADIC tokens uuid[])
  RETURNS UUID AS
$$
DECLARE
  times_token uuid;
  filtered_tokens uuid[];
  canonical uuid;
BEGIN
  -- A NULL element reads as the ⊗-neutral 1: it is the token slot of an
  -- untracked source (a join against an untracked table), which is
  -- certain.  Contrast provenance_plus / provenance_monus, where NULL
  -- reads as the ⊕- / ⊖-right-neutral 0: each combinator maps NULL to
  -- its own neutral element.  Nothing may therefore hand a NULL to ⊗
  -- meaning "false"; a comparison with a NULL operand goes through
  -- provenance_cmp, which returns gate_zero for it.
  SELECT array_agg(t) FROM unnest(tokens) t WHERE t IS NOT NULL AND t <> gate_one() INTO filtered_tokens;

  -- Dispatch on the FILTERED count: a single survivor short-circuits
  -- to that token directly (no useless single-child times gate); zero
  -- survivors collapse to the identity. Using array_length(tokens, 1)
  -- here would miss the [one, cmp] → [cmp] case, leaving the cmp wrapped
  -- in a one-child times when its only sibling was gate_one().
  CASE coalesce(array_length(filtered_tokens, 1), 0)
    WHEN 0 THEN
      times_token:=gate_one();
    WHEN 1 THEN
      times_token:=filtered_tokens[1];
    ELSE
      -- Computed separately from the filtering aggregate above: an
      -- ORDER BY aggregate there would make the planner feed *both*
      -- aggregates sorted input, scrambling the stored children order.
      SELECT uuid_generate_v5(uuid_ns_provsql(),
                              concat('times-canonical', array_agg(t ORDER BY t)))
      FROM unnest(filtered_tokens) t
      INTO canonical;
      IF get_gate_type(canonical) = 'times' THEN
        -- A deliberate pre-creation at the canonical address: same
        -- children, same product.
        times_token := canonical;
      ELSE
        times_token := uuid_generate_v5(uuid_ns_provsql(),concat('times',filtered_tokens));

        PERFORM create_gate(times_token, 'times', ARRAY_AGG(t)) FROM UNNEST(filtered_tokens) AS t WHERE t IS NOT NULL;
      END IF;
  END CASE;

  RETURN times_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_delta
  (token UUID)
  RETURNS UUID AS
$$
DECLARE
  delta_token uuid;
BEGIN
  -- NULL token ≡ 1 (untracked source), and δ(1) = 1.  Tested first: the
  -- equality comparisons below are not NULL-safe.
  IF token IS NULL THEN
    return gate_one();
  END IF;

  IF token = gate_zero() OR token = gate_one() THEN
    return token;
  END IF;

  delta_token:=uuid_generate_v5(uuid_ns_provsql(),concat('delta',token));

  PERFORM create_gate(delta_token,'delta',ARRAY[token::uuid]);

  RETURN delta_token;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_aggregate(
    aggfnoid integer,
    aggtype integer,
    val anyelement,
    tokens uuid[],
    is_scalar boolean DEFAULT false)
  RETURNS agg_token AS
$$
DECLARE
  c INTEGER;
  agg_tok uuid;
  agg_val varchar;
BEGIN
  -- Drop the NULL placeholders array_agg keeps for rows that did not produce a
  -- semimod gate (provenance_semimod returns NULL for a NULL aggregated value),
  -- so a NULL input never participates in the aggregate.
  tokens := array_remove(tokens, NULL);
  c:=COALESCE(array_length(tokens, 1), 0);

  agg_val = CAST(val as VARCHAR);

  IF c = 0 THEN
    agg_tok := gate_zero();
  ELSE
    -- aggfnoid must be part of the UUID: SUM(id) and AVG(id) over the
    -- same children would otherwise collapse to a single gate, and
    -- their concurrent set_infos calls would overwrite each other's
    -- aggregation operator (resulting in the wrong agg_kind being
    -- read by provsql_having under cross-backend contention).  The
    -- scalar-aggregation flag must likewise be hashed: a scalar and a
    -- grouped aggregate over identical children carry different info2 and
    -- must stay distinct gates, else the concurrent set_infos calls would
    -- clobber the flag.  The flag is stored in the high bit of info2 (the
    -- low 31 bits keep the result-type OID); aggtype itself is passed clean
    -- so the agg_token->scalar cast still finds a valid type.
    agg_tok := uuid_generate_v5(
      uuid_ns_provsql(),
      concat('agg',aggfnoid,tokens,CASE WHEN is_scalar THEN 'S' ELSE '' END));
    PERFORM create_gate(agg_tok, 'agg', tokens);
    PERFORM set_infos(agg_tok, aggfnoid,
                      CASE WHEN is_scalar THEN aggtype | (-2147483648) ELSE aggtype END);
    PERFORM set_extra(agg_tok, agg_val);
  END IF;

  RETURN '( '||agg_tok||' , '||agg_val||' )';
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql,pg_temp,public SECURITY DEFINER IMMUTABLE;

CREATE OR REPLACE FUNCTION provenance_semimod(val anyelement, token UUID)
  RETURNS UUID AS
$$
DECLARE
  semimod_token uuid;
  value_token uuid;
BEGIN
  -- A NULL value means this row does not participate in the aggregate (SQL
  -- aggregates ignore NULL inputs; only count(*) counts rows unconditionally,
  -- and it passes a constant 1 here).  Produce no semimod gate so the row is
  -- skipped when provenance_aggregate builds the agg gate.
  IF val IS NULL THEN
    RETURN NULL;
  END IF;

  SELECT uuid_generate_v5(uuid_ns_provsql(),concat('value',CAST(val AS VARCHAR)))
    INTO value_token;
  SELECT uuid_generate_v5(uuid_ns_provsql(),concat('semimod',value_token,token))
    INTO semimod_token;

  --create value gates
  PERFORM create_gate(value_token,'value');
  PERFORM set_extra(value_token, CAST(val AS VARCHAR));

  --create semimod gate
  PERFORM create_gate(semimod_token,'semimod',ARRAY[token::uuid,value_token]);

  RETURN semimod_token;
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql,pg_temp,public SECURITY DEFINER IMMUTABLE;

CREATE OR REPLACE FUNCTION agg_token_to_random_variable(a agg_token)
  RETURNS random_variable AS
$$ SELECT provsql.random_variable_make(provsql.agg_token_uuid($1)); $$
  LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_cast c
      JOIN pg_type s ON s.oid = c.castsource
      JOIN pg_type t ON t.oid = c.casttarget
    WHERE s.typname = 'agg_token' AND t.typname = 'random_variable'
  ) THEN
    CREATE CAST (agg_token AS random_variable)
      WITH FUNCTION agg_token_to_random_variable(agg_token) AS IMPLICIT;
  END IF;
END $$;

CREATE OR REPLACE FUNCTION rv_parametric2(
    family text,
    p1_tok uuid, p1_lit double precision,
    p2_tok uuid, p2_lit double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
  wires uuid[] := ARRAY[]::uuid[];
  s1 text;
  s2 text;
BEGIN
  IF p1_tok IS NOT NULL THEN
    wires := wires || p1_tok;
    s1 := '$' || (array_length(wires, 1) - 1);
  ELSE
    IF NOT provsql.is_finite_float8(p1_lit) THEN
      RAISE EXCEPTION 'provsql.%: literal parameter must be finite (got %)',
        family, p1_lit;
    END IF;
    s1 := p1_lit::text;
  END IF;
  IF p2_tok IS NOT NULL THEN
    wires := wires || p2_tok;
    s2 := '$' || (array_length(wires, 1) - 1);
  ELSE
    IF NOT provsql.is_finite_float8(p2_lit) THEN
      RAISE EXCEPTION 'provsql.%: literal parameter must be finite (got %)',
        family, p2_lit;
    END IF;
    s2 := p2_lit::text;
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', wires);
  PERFORM provsql.set_extra(token, family || ':' || s1 || ',' || s2);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rv_parametric1(family text, p_tok uuid)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv', ARRAY[p_tok]);
  PERFORM provsql.set_extra(token, family || ':$0');
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION normal(mu random_variable, sigma double precision)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('normal', ($1)::uuid, NULL, NULL, $2); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION normal(mu double precision, sigma random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('normal', NULL, $1, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION normal(mu random_variable, sigma random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('normal', ($1)::uuid, NULL, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION logistic(mu random_variable, s double precision)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('logistic', ($1)::uuid, NULL, NULL, $2); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION logistic(mu double precision, s random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('logistic', NULL, $1, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION logistic(mu random_variable, s random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('logistic', ($1)::uuid, NULL, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION uniform(a random_variable, b double precision)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('uniform', ($1)::uuid, NULL, NULL, $2); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION uniform(a double precision, b random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('uniform', NULL, $1, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION uniform(a random_variable, b random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('uniform', ($1)::uuid, NULL, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION exponential(lambda random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric1('exponential', ($1)::uuid); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gamma(k random_variable, lambda double precision)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('gamma', ($1)::uuid, NULL, NULL, $2); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gamma(k double precision, lambda random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('gamma', NULL, $1, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION gamma(k random_variable, lambda random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('gamma', ($1)::uuid, NULL, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION lognormal(mu random_variable, sigma double precision)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('lognormal', ($1)::uuid, NULL, NULL, $2); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION lognormal(mu double precision, sigma random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('lognormal', NULL, $1, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION lognormal(mu random_variable, sigma random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('lognormal', ($1)::uuid, NULL, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION weibull(k random_variable, lambda double precision)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('weibull', ($1)::uuid, NULL, NULL, $2); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION weibull(k double precision, lambda random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('weibull', NULL, $1, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION weibull(k random_variable, lambda random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('weibull', ($1)::uuid, NULL, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION pareto(xm random_variable, alpha double precision)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('pareto', ($1)::uuid, NULL, NULL, $2); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION pareto(xm double precision, alpha random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('pareto', NULL, $1, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION pareto(xm random_variable, alpha random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('pareto', ($1)::uuid, NULL, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION beta(alpha random_variable, beta double precision)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('beta', ($1)::uuid, NULL, NULL, $2); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION beta(alpha double precision, beta random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('beta', NULL, $1, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION beta(alpha random_variable, beta random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('beta', ($1)::uuid, NULL, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION inverse_gamma(alpha random_variable, beta double precision)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('inverse_gamma', ($1)::uuid, NULL, NULL, $2); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION inverse_gamma(alpha double precision, beta random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('inverse_gamma', NULL, $1, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION inverse_gamma(alpha random_variable, beta random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('inverse_gamma', ($1)::uuid, NULL, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION inverse_gaussian(mu random_variable, lambda double precision)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('inverse_gaussian', ($1)::uuid, NULL, NULL, $2); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION inverse_gaussian(mu double precision, lambda random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('inverse_gaussian', NULL, $1, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION inverse_gaussian(mu random_variable, lambda random_variable)
  RETURNS random_variable AS $$ SELECT provsql.rv_parametric2('inverse_gaussian', ($1)::uuid, NULL, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION logistic(mu double precision, s double precision)
  RETURNS random_variable AS
$$
DECLARE
  token uuid;
BEGIN
  IF NOT provsql.is_finite_float8(mu) OR NOT provsql.is_finite_float8(s) THEN
    RAISE EXCEPTION 'provsql.logistic: parameters must be finite (got mu=%, s=%)', mu, s;
  END IF;
  IF s < 0 THEN
    RAISE EXCEPTION 'provsql.logistic: scale s must be non-negative (got %)', s;
  END IF;
  IF s = 0 THEN
    RETURN provsql.as_random(mu);
  END IF;
  token := public.uuid_generate_v4();
  PERFORM provsql.create_gate(token, 'rv');
  PERFORM provsql.set_extra(token, 'logistic:' || mu || ',' || s);
  RETURN provsql.random_variable_make(token);
END
$$ LANGUAGE plpgsql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION poisson(lambda random_variable)
  RETURNS random_variable AS
$$ SELECT provsql.rv_parametric1('poisson', ($1)::uuid); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION binomial(n integer, p random_variable)
  RETURNS random_variable AS
$$ SELECT provsql.rv_parametric2('binomial', NULL, $1::double precision,
                                 ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION geometric(p random_variable)
  RETURNS random_variable AS
$$ SELECT provsql.rv_parametric1('geometric', ($1)::uuid); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION negative_binomial(r double precision, p random_variable)
  RETURNS random_variable AS
$$ SELECT provsql.rv_parametric2('negative_binomial', NULL, $1, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION negative_binomial(r random_variable, p double precision)
  RETURNS random_variable AS
$$ SELECT provsql.rv_parametric2('negative_binomial', ($1)::uuid, NULL, NULL, $2); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION negative_binomial(r random_variable, p random_variable)
  RETURNS random_variable AS
$$ SELECT provsql.rv_parametric2('negative_binomial', ($1)::uuid, NULL, ($2)::uuid, NULL); $$
  LANGUAGE sql STRICT VOLATILE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION random_variable_cond(rv random_variable, cond uuid)
  RETURNS random_variable AS
$$
DECLARE
  tgt uuid;
  ev  uuid;
  result uuid;
  ch uuid[];
BEGIN
  IF cond IS NULL OR cond = gate_one() THEN
    RETURN rv;
  END IF;

  -- A point-equality "Y = c" on a bare random-variable leaf is an
  -- OBSERVATION, not a truncation: rewrite it to the internal likelihood-
  -- weighting evidence (its density / mass at c).  This is what lets
  -- "X | (normal(mu,1) = 8)" (a continuous point event, measure-zero as a
  -- Boolean selection) condition as the disintegration rather than fold to
  -- an infeasible event.
  cond := provsql.evidence_as_observation(cond);

  tgt := (rv)::uuid;
  IF get_gate_type(tgt) = 'conditioned'
     AND array_length(get_children(tgt), 1) = 2 THEN
    -- Fold (X|A)|B = X|(A∧B): the rv-carrier conditioned gate is the
    -- two-child [target, condition] shape; accumulate the new event.
    ch  := get_children(tgt);
    tgt := ch[1];
    ev  := provenance_times(ch[2], cond);
  ELSE
    ev := cond;
  END IF;

  result := public.uuid_generate_v5(uuid_ns_provsql(),
                                    concat('conditioned', tgt, ev));
  PERFORM create_gate(result, 'conditioned', ARRAY[tgt, ev]);
  RETURN (result)::random_variable;
END
$$ LANGUAGE plpgsql SET search_path=provsql,pg_temp,public
   SECURITY DEFINER PARALLEL SAFE;

CREATE OR REPLACE FUNCTION given(evidence UUID) RETURNS UUID AS
$$
BEGIN
  RETURN provsql.evidence_as_observation(evidence);
END
$$ LANGUAGE plpgsql VOLATILE PARALLEL SAFE
   SET search_path=provsql,pg_temp,public SECURITY DEFINER;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o
      JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '|'
      AND o.oprleft = 0
      AND o.oprright = 'pg_catalog.uuid'::regtype::oid
      AND o.oprcode <> 0
  ) THEN
    CREATE OPERATOR | (RIGHTARG=UUID, PROCEDURE=given);
  END IF;
END $$;

CREATE OR REPLACE FUNCTION given(predicate boolean) RETURNS UUID AS
$$
BEGIN
  RAISE EXCEPTION 'given(predicate) / prefix | (predicate) must be rewritten '
    'by the ProvSQL planner hook: the operand must be a Boolean combination '
    'of random_variable / aggregate comparisons (is provsql.active off?)';
END
$$ LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_operator o
      JOIN pg_namespace n ON n.oid = o.oprnamespace
    WHERE n.nspname = 'provsql' AND o.oprname = '|'
      AND o.oprleft = 0
      AND o.oprright = 'pg_catalog.bool'::regtype::oid
      AND o.oprcode <> 0
  ) THEN
    CREATE OPERATOR | (RIGHTARG=boolean, PROCEDURE=given);
  END IF;
END $$;

CREATE OR REPLACE FUNCTION evidence_as_observation(ev uuid) RETURNS uuid AS
$$
DECLARE
  ch uuid[];
  i1 integer;
  leaf uuid;
  datum_gate uuid;
BEGIN
  IF ev IS NULL OR provsql.get_gate_type(ev) <> 'cmp' THEN
    RETURN ev;
  END IF;
  ch := provsql.get_children(ev);
  IF array_length(ch, 1) <> 2 THEN
    RETURN ev;
  END IF;
  -- The cmp stores the comparison OPERATOR's OID in info1; match on its name
  -- '=' the same way the C-side cmpOpFromOid does (get_opname), rather than a
  -- fixed operator OID (which varies per install / carrier type).
  SELECT info1 INTO i1 FROM provsql.get_infos(ev);
  IF (SELECT oprname FROM pg_catalog.pg_operator WHERE oid = i1) IS DISTINCT FROM '=' THEN
    RETURN ev;   -- not an equality
  END IF;
  IF provsql.get_gate_type(ch[1]) = 'rv'
     AND provsql.get_gate_type(ch[2]) = 'value' THEN
    leaf := ch[1]; datum_gate := ch[2];
  ELSIF provsql.get_gate_type(ch[2]) = 'rv'
        AND provsql.get_gate_type(ch[1]) = 'value' THEN
    leaf := ch[2]; datum_gate := ch[1];
  ELSE
    RETURN ev;   -- not a bare-leaf-vs-constant point event
  END IF;
  RETURN provsql.observe((leaf)::random_variable,
                         provsql.get_extra(datum_gate)::double precision);
END
$$ LANGUAGE plpgsql VOLATILE
   SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE;

CREATE OR REPLACE FUNCTION observe(x random_variable, datum double precision)
  RETURNS uuid AS
$$
DECLARE
  leaf uuid := (x)::uuid;
  result uuid;
BEGIN
  IF provsql.get_gate_type(leaf) <> 'rv' THEN
    RAISE EXCEPTION 'provsql.observe: the argument must be a bare '
      'random-variable leaf (a gate_rv), got a % gate', provsql.get_gate_type(leaf)
      USING HINT = 'observe binds a datum to a single distribution leaf; '
        'observing a derived quantity (a sum, product, or comparison) needs '
        'a change-of-variables density and is out of scope.';
  END IF;
  IF NOT provsql.is_finite_float8(datum) THEN
    RAISE EXCEPTION 'provsql.observe: datum must be finite (got %)', datum;
  END IF;
  result := public.uuid_generate_v4();
  PERFORM provsql.create_gate(result, 'observe', ARRAY[leaf]);
  PERFORM provsql.set_extra(result, datum::text);
  RETURN result;
END
$$ LANGUAGE plpgsql VOLATILE
   SET search_path=provsql,pg_temp,public SECURITY DEFINER PARALLEL SAFE;

CREATE OR REPLACE FUNCTION and_agg_sfunc(state uuid, ev uuid)
  RETURNS uuid AS
$$
  SELECT provsql.provenance_times(state, ev);
$$ LANGUAGE sql PARALLEL SAFE;

DO $$ BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_aggregate a
      JOIN pg_proc p ON p.oid = a.aggfnoid
      JOIN pg_namespace n ON n.oid = p.pronamespace
    WHERE n.nspname = 'provsql' AND p.proname = 'and_agg'
      AND p.pronargs = 1
      AND p.proargtypes[0] = 'pg_catalog.uuid'::regtype::oid
  ) THEN
    CREATE AGGREGATE and_agg(uuid) (
      SFUNC = and_agg_sfunc,
      STYPE = uuid
    );
  END IF;
END $$;

CREATE OR REPLACE FUNCTION evidence(evidence uuid)
  RETURNS double precision
  AS 'provsql','rv_evidence' LANGUAGE C STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION observe_atoms(evidence uuid)
  RETURNS uuid[] AS
$$
  WITH RECURSIVE walk(tok) AS (
    SELECT evidence
    UNION
    SELECT c
    FROM walk, LATERAL unnest(provsql.get_children(walk.tok)) AS c
    WHERE provsql.get_gate_type(walk.tok) = 'times'
  )
  SELECT array_agg(tok ORDER BY tok)
  FROM walk
  WHERE provsql.get_gate_type(tok) = 'observe';
$$ LANGUAGE sql STABLE PARALLEL SAFE SET search_path=provsql,pg_temp,public;

CREATE OR REPLACE FUNCTION shapley_observe(
    target uuid, evidence uuid, payoff text DEFAULT 'expected')
  RETURNS TABLE(observation uuid, value double precision) AS
$$
DECLARE
  atoms uuid[];
  n int;
  nmasks int;
  pv double precision[];
  popc int[];
  fact double precision[];
  mask int;
  i int;
  j int;
  cnt int;
  subset uuid[];
  ev_s uuid;
  sh double precision;
  bit int;
  s_size int;
BEGIN
  IF payoff NOT IN ('expected', 'variance') THEN
    RAISE EXCEPTION 'provsql.shapley_observe: payoff must be ''expected'' or '
      '''variance'' (got %)', payoff;
  END IF;
  atoms := provsql.observe_atoms(evidence);
  n := coalesce(array_length(atoms, 1), 0);
  IF n = 0 THEN
    RAISE EXCEPTION 'provsql.shapley_observe: evidence contains no observe() '
      'atoms (got a % gate)', provsql.get_gate_type(evidence);
  END IF;
  IF n > 12 THEN
    RAISE EXCEPTION 'provsql.shapley_observe: exact attribution over % '
      'observations is exponential; capped at 12 (sampling-based '
      'attribution is future work)', n;
  END IF;

  -- factorials 0!..n!  (fact[k+1] = k!)
  fact := ARRAY[1::double precision];
  FOR i IN 1..n LOOP fact := fact || (fact[i] * i); END LOOP;

  nmasks := (1 << n);
  pv   := array_fill(NULL::double precision, ARRAY[nmasks]);
  popc := array_fill(0, ARRAY[nmasks]);

  -- Payoff value function for every subset of observations.
  FOR mask IN 0 .. nmasks - 1 LOOP
    subset := ARRAY[]::uuid[];
    cnt := 0;
    FOR i IN 0 .. n - 1 LOOP
      IF (mask >> i) & 1 = 1 THEN
        subset := subset || atoms[i + 1];
        cnt := cnt + 1;
      END IF;
    END LOOP;
    popc[mask + 1] := cnt;
    IF cnt = 0 THEN
      ev_s := provsql.gate_one();              -- prior (no evidence)
    ELSE
      ev_s := provsql.provenance_times(VARIADIC subset);
    END IF;
    IF payoff = 'expected' THEN
      pv[mask + 1] := provsql.rv_moment(target, 1, false, ev_s);
    ELSE
      pv[mask + 1] := provsql.rv_moment(target, 2, true, ev_s);
    END IF;
  END LOOP;

  -- Shapley value of each observation atom.
  FOR i IN 0 .. n - 1 LOOP
    sh := 0;
    bit := (1 << i);
    FOR mask IN 0 .. nmasks - 1 LOOP
      IF (mask >> i) & 1 = 0 THEN               -- subsets S not containing i
        s_size := popc[mask + 1];
        -- weight |S|! (n-|S|-1)! / n!
        sh := sh + (fact[s_size + 1] * fact[n - s_size] / fact[n + 1])
                 * (pv[(mask | bit) + 1] - pv[mask + 1]);
      END IF;
    END LOOP;
    observation := atoms[i + 1];
    value := sh;
    RETURN NEXT;
  END LOOP;
END
$$ LANGUAGE plpgsql VOLATILE
   SET search_path=provsql,pg_temp,public SECURITY DEFINER;

CREATE OR REPLACE FUNCTION agg_collapsed_moment(token uuid, k integer)
  RETURNS double precision
  AS 'provsql','agg_collapsed_moment' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_collapsed_moments(token uuid)
  RETURNS double precision[]
  AS 'provsql','agg_collapsed_moments' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION agg_raw_moment(
  token agg_token,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  aggregation_function VARCHAR;
  child_pairs uuid[];
  pair_children uuid[];
  n integer;
  i integer;
  j integer;
  vals float8[];
  toks uuid[];
  total float8;
  total_probability float8;
  tup integer[];
  d integer;
  prod_v float8;
  distinct_tok uuid[];
  conj_token uuid;
  prob float8;
  sign_max float8;
BEGIN
  IF token IS NULL OR k IS NULL THEN
    RETURN NULL;
  END IF;
  IF k < 0 THEN
    RAISE EXCEPTION 'agg_raw_moment(): k must be non-negative (got %)', k;
  END IF;

  -- Aggregate-carrier CASE (a gate_case over aggregate branches): a first-match
  -- guarded selection.  The moment is CONDITIONAL on the CASE's value being
  -- defined (NULL only when it never is, mirroring the MIN/MAX convention):
  --   E[pick^k | defined ∧ prov]
  --     = Σ_i P(region_i ∧ def_i) · E[value_i^k | region_i ∧ def_i]
  --       / Σ_i P(region_i ∧ def_i),
  -- where region_i = (¬g_1 ∧ … ∧ ¬g_{i-1}) ∧ g_i ∧ prov is the world set that
  -- selects branch i (the default's region is "all guards false") and def_i is
  -- the branch's defined event (agg_defined_event: gate_one for sum / count /
  -- constants, "some row present" for min / max / avg, recursive for a nested
  -- CASE).  Both factors are exact: probability() over the region ∧ def event,
  -- and the conditional aggregate moment (a recursive agg_raw_moment on the
  -- branch aggregate, which conditions on its own definedness within the
  -- region, so the two factors weigh the same worlds).  The regions are
  -- mutually exclusive, so the terms sum with no inclusion-exclusion, and
  -- correlation between a guard and its branch (shared input tuples) is
  -- carried by the conditioning, exactly as HAVING carries it.  When every
  -- branch is defined everywhere, the defined mass equals P(prov) and the
  -- formula reduces to the plain region-weighted sum.
  IF get_gate_type(token) = 'case' THEN
    IF k = 0 THEN
      RETURN 1;
    END IF;
    DECLARE
      wires uuid[] := get_children(token);
      nw integer := array_length(get_children(token), 1);
      m integer := (array_length(get_children(token), 1) - 1) / 2;
      running_neg uuid := gate_one();
      region_full uuid;
      prov_p float8;
      p float8;
      total float8 := 0;
      def_mass float8 := 0;
      ci integer;
      vuid uuid;
      bm float8;
    BEGIN
      prov_p := probability(prov);
      IF prov_p IS NULL OR prov_p <= 0 THEN
        RETURN NULL;   -- impossible conditioning event
      END IF;
      -- Branches 1..m are the guarded WHENs; branch m+1 is the ELSE default,
      -- whose region is "all guards false".
      FOR ci IN 1 .. m + 1 LOOP
        IF ci <= m THEN
          region_full := provenance_times(running_neg, wires[2 * ci - 1], prov);
          vuid := wires[2 * ci];
          running_neg :=
            provenance_times(running_neg, provenance_not(wires[2 * ci - 1]));
        ELSE
          region_full := provenance_times(running_neg, prov);
          vuid := wires[nw];
        END IF;
        p := probability(provenance_times(region_full,
                                          agg_defined_event(vuid)));
        IF p > 0 THEN
          -- E[value_i^k | region_i ∧ def_i]: a constant branch is a Dirac
          -- (c^k, exact); a single aggregate or nested CASE is exact via
          -- agg_raw_moment (whose MIN/MAX/CASE arms condition on their own
          -- definedness within the region); an arithmetic / composite branch
          -- takes the Monte-Carlo scalar path (which composes with the
          -- aggregate leaves).
          IF get_gate_type(vuid) = 'value' THEN
            bm := power(CAST(get_extra(vuid) AS float8), k);
          ELSIF get_gate_type(vuid) IN ('agg', 'case') THEN
            bm := agg_raw_moment(agg_token_make(vuid, 0), k, region_full,
                                 method, arguments);
          ELSE
            bm := rv_moment(vuid, k, false, region_full);
          END IF;
          total := total + p * bm;
          def_mass := def_mass + p;
        END IF;
      END LOOP;
      IF def_mass <= epsilon() THEN
        RETURN NULL;   -- the CASE's value is never defined under prov
      END IF;
      RETURN total / def_mass;
    END;
  END IF;

  IF get_gate_type(token) <> 'agg' THEN
    IF get_gate_type(token) IN ('arith', 'conditioned') THEN
      RAISE EXCEPTION 'expected / variance / moment over an arithmetic '
        'combination of aggregates (e.g. SUM(x) + SUM(y) or SUM(x) + 5), or a '
        'conditioning of one, is not yet supported: a moment can be taken only '
        'over a single aggregate (SUM / COUNT / MIN / MAX), optionally '
        'conditioned (SUM(x) | C)'
        USING HINT = 'Take the moment of each aggregate separately, or condition '
          'the bare aggregate.';
    ELSE
      RAISE EXCEPTION USING MESSAGE='Wrong gate type for agg_raw_moment computation';
    END IF;
  END IF;
  IF k = 0 THEN
    RETURN 1;
  END IF;

  SELECT pp.proname::varchar FROM pg_proc pp
    WHERE oid=(get_infos(token)).info1
    INTO aggregation_function;

  child_pairs := get_children(token);
  n := COALESCE(array_length(child_pairs, 1), 0);

  IF aggregation_function = 'sum' OR aggregation_function = 'count' THEN
    -- count(col) keeps the COUNT identity at the gate level but its value is a
    -- SUM of per-row 0/1 indicators, so its moments are computed exactly like
    -- SUM (and its empty group is the real value 0, like SUM).  count(*)
    -- arrives here as 'sum' (it normalises to F_SUM_INT4); count(col) as 'count'.
    -- Trivial empty aggregation: SUM = 0, so SUM^k = 0 for k >= 1.
    -- Note: agg_token semantics treat the "no row included" world as
    -- SUM = 0, so this stays consistent with k = 1 (= expected()).
    IF n = 0 THEN
      RETURN 0;
    END IF;

    -- Collapsed fast path: a correlated COUNT / SUM whose per-row selection
    -- events share a single continuous latent has an O(G·n) 1-D quadrature,
    -- vastly cheaper than the O(n^k) tuple enumeration below (which is the
    -- O(n^2) pair-probability bottleneck for the variance).  Only fires
    -- unconditionally (prov = one) and for k in {1, 2}; agg_collapsed_moment
    -- returns NULL when the shared-latent pattern does not match, and we
    -- fall through to the exact enumeration.
    IF prov = gate_one() AND k <= 2 THEN
      total := agg_collapsed_moment((token)::uuid, k);
      IF total IS NOT NULL THEN
        RETURN total;
      END IF;
    END IF;

    -- Extract per-child token + value arrays.
    vals := ARRAY[]::float8[];
    toks := ARRAY[]::uuid[];
    FOR i IN 1..n LOOP
      pair_children := get_children(child_pairs[i]);
      toks := toks || pair_children[1];
      vals := vals || CAST(get_extra(pair_children[2]) AS float8);
    END LOOP;

    -- Enumerate all k-tuples (i_1, ..., i_k) in {1..n}^k.  tup is the
    -- current tuple; we step through them in lexicographic order.
    total := 0;
    tup := array_fill(1, ARRAY[k]);
    LOOP
      prod_v := 1;
      FOR j IN 1..k LOOP
        prod_v := prod_v * vals[tup[j]];
      END LOOP;

      SELECT array_agg(DISTINCT toks[idx]) INTO distinct_tok
        FROM unnest(tup) AS idx;

      IF prov <> gate_one() THEN
        distinct_tok := distinct_tok || prov;
      END IF;
      conj_token := provenance_times(VARIADIC distinct_tok);
      prob := probability_evaluate(conj_token, method, arguments);

      total := total + prod_v * prob;

      d := k;
      WHILE d >= 1 AND tup[d] = n LOOP
        tup[d] := 1;
        d := d - 1;
      END LOOP;
      EXIT WHEN d = 0;
      tup[d] := tup[d] + 1;
    END LOOP;
  ELSIF aggregation_function = 'min' OR aggregation_function = 'max' THEN
    -- Rank enumeration: per distinct value v, P(MIN = v) is the
    -- probability that some t_i with v_i=v is true and all t_j with
    -- smaller v are false.  For MAX we negate values so the same
    -- "smaller-than" rank logic computes MIN-of-negated, then flip.
    -- The outer multiplier picks up the right sign for the k-th moment
    -- of MAX: E[MAX^k] = (-1)^k * E[MIN(-v)^k], so sign_max = (-1)^k.
    sign_max := CASE
                  WHEN aggregation_function = 'max'
                  THEN power(-1::float8, k)
                  ELSE 1
                END;

    -- MIN/MAX over the empty input world are NULL (no elements), not ±Infinity:
    -- SQL returns one row with a NULL value.  The moment is therefore CONDITIONAL
    -- on the aggregate being defined (non-empty) -- the empty world is excluded
    -- and the result renormalised by P(prov AND non-empty).  (count, whose empty
    -- value 0 is a real value, keeps the empty world; sum keeps it too, as 0.)
    IF n = 0 THEN
      RETURN NULL;  -- structurally empty: MIN/MAX undefined
    END IF;

    -- Numerator E[MIN^k . 1{prov AND non-empty}] (the rank sum naturally omits
    -- the empty world, since every term requires a present token).
    WITH tok_value AS (
      SELECT (get_children(c))[1] AS tok,
             (CASE WHEN aggregation_function='max' THEN -1 ELSE 1 END)
               * CAST(get_extra((get_children(c))[2]) AS DOUBLE PRECISION) AS v
      FROM UNNEST(child_pairs) AS c
    ) SELECT sign_max * COALESCE(SUM(p * power(v, k)), 0) FROM (
        SELECT t1.v AS v,
          probability_evaluate(
            CASE WHEN prov = gate_one()
                 THEN provenance_monus(provenance_plus(ARRAY_AGG(t1.tok)),
                                       provenance_plus(ARRAY_AGG(t2.tok)))
                 ELSE provenance_times(prov,
                        provenance_monus(provenance_plus(ARRAY_AGG(t1.tok)),
                                         provenance_plus(ARRAY_AGG(t2.tok)))) END,
            method, arguments) AS p
        FROM tok_value t1 LEFT OUTER JOIN tok_value t2 ON t1.v > t2.v
        GROUP BY t1.v) tmp
      INTO total;

    -- Denominator P(prov AND non-empty) = P(prov (x) (+) tokens).
    SELECT probability_evaluate(
             CASE WHEN prov = gate_one()
                  THEN provenance_plus(ARRAY_AGG(tok))
                  ELSE provenance_times(prov, provenance_plus(ARRAY_AGG(tok))) END,
             method, arguments)
      FROM (SELECT (get_children(c))[1] AS tok FROM UNNEST(child_pairs) AS c) s
      INTO total_probability;

    IF total_probability <= epsilon() THEN
      RETURN NULL;  -- never defined under prov: MIN/MAX undefined
    END IF;
    RETURN total / total_probability;  -- already conditional; skip generic norm
  ELSIF aggregation_function = 'avg' THEN
    -- AVG = SUM/COUNT is a ratio of two correlated world-dependent
    -- quantities, so the k-tuple expansion above does not apply.  Like
    -- MIN/MAX, AVG over the empty world is NULL, so its moment conditions
    -- on the aggregate being defined (COUNT >= 1), NULL when it never is.
    -- Two routes:
    --  * EXACT (independent rows, unconditional): the joint (sum, count)
    --    PMF folded in C by agg_avg_moment_exact --
    --    E[AVG^k | COUNT>=1] = Σ_{(s,c), c>=1} (s/c)^k pmf(s,c) / P(c>=1).
    --  * Monte-Carlo scalar fallback otherwise (an outer conditioning
    --    event, shared leaves, compound contributors): rv_moment samples
    --    the agg gate per world; its NaN-skip on empty draws implements
    --    the same conditional-on-defined convention, at the
    --    provsql.rv_mc_samples budget (0 raises, per convention).
    IF n = 0 THEN
      RETURN NULL;  -- structurally empty: AVG undefined
    END IF;
    IF prov = gate_one() THEN
      total := agg_avg_moment_exact((token)::uuid, k);
      IF total IS NOT NULL THEN
        RETURN total;
      END IF;
    END IF;
    RETURN rv_moment((token)::uuid, k, false, prov);
  ELSE
    RAISE EXCEPTION USING MESSAGE=
      'Cannot compute moment for aggregation function ' || aggregation_function;
  END IF;

  -- Conditional normalisation: E[X^k · 1_A] / P(A) = E[X^k | A].
  IF prov <> gate_one()
     AND total <> 0
     AND total <> 'Infinity'::float8
     AND total <> '-Infinity'::float8 THEN
    total := total / probability_evaluate(prov, method, arguments);
  END IF;

  RETURN total;
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION variance(
  input ANYELEMENT,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  m1 float8;
  m2 float8;
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL THEN
      RETURN NULL;
    END IF;
    -- Conditioning on prov is handled inside rv_moment: when prov
    -- resolves to gate_one() (the default, or load-time
    -- simplification of any always-true sub-circuit) the
    -- unconditional analytical path runs unchanged; otherwise the
    -- joint-circuit loader unifies shared gate_rv leaves between
    -- input and prov, and the conditional path runs either
    -- truncated-distribution closed form or MC rejection.
    RETURN provsql.rv_moment(
      rv_conditioned_target((input::random_variable)::uuid), 2, true,
      rv_conditioned_prov((input::random_variable)::uuid, prov));
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    IF input IS NULL THEN
      RETURN NULL;
    END IF;
    -- Collapsed fast path: E[C] and E[C^2] from a single circuit load and plan
    -- build, instead of two agg_raw_moment() calls that each reload.  Mirrors
    -- the guard in agg_raw_moment (unconditional only, prov = one); on any
    -- mismatch agg_collapsed_moments returns NULL and we fall through to the
    -- generic per-order path (which handles conditioning, SUM enumeration, ...).
    IF rv_conditioned_prov(input::uuid, prov) = gate_one() THEN
      DECLARE ms float8[];
      BEGIN
        ms := agg_collapsed_moments(
                (agg_conditioned_target(input::agg_token))::uuid);
        IF ms IS NOT NULL THEN
          RETURN ms[2] - ms[1] * ms[1];
        END IF;
      END;
    END IF;
    m1 := agg_raw_moment(agg_conditioned_target(input::agg_token), 1,
                         rv_conditioned_prov(input::uuid, prov), method, arguments);
    m2 := agg_raw_moment(agg_conditioned_target(input::agg_token), 2,
                         rv_conditioned_prov(input::uuid, prov), method, arguments);
    IF m1 IS NULL OR m2 IS NULL THEN
      RETURN NULL;
    END IF;
    RETURN m2 - m1 * m1;
  END IF;

  -- Bernoulli event token (see moment()): Var[X] = p(1 - p).
  IF pg_typeof(input) = 'uuid'::regtype THEN
    IF input IS NULL THEN
      RETURN NULL;
    END IF;
    m1 := provsql.probability_evaluate(provsql.cond(input::uuid, prov),
                                       method, arguments);
    RETURN m1 * (1 - m1);
  END IF;

  RAISE EXCEPTION 'variance() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION moment(
  input ANYELEMENT,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    -- See variance() above: rv_moment handles the conditional/unconditional
    -- dispatch internally based on the resolved prov gate type.
    RETURN provsql.rv_moment(
      rv_conditioned_target((input::random_variable)::uuid), k, false,
      rv_conditioned_prov((input::random_variable)::uuid, prov));
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    RETURN agg_raw_moment(agg_conditioned_target(input::agg_token), k,
                          rv_conditioned_prov(input::uuid, prov), method, arguments);
  END IF;

  -- A bare provenance event token (a gate_cmp lifted from an RV comparison,
  -- e.g. expected(x <= c)) is a Bernoulli indicator: X in {0,1}, so every raw
  -- moment E[X^k] with k >= 1 equals P(event), and E[X^0] = 1.  cond() applies
  -- the optional conditioning prov (a no-op for the default gate_one()).
  IF pg_typeof(input) = 'uuid'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    IF k = 0 THEN
      RETURN 1;
    END IF;
    RETURN provsql.probability_evaluate(provsql.cond(input::uuid, prov),
                                        method, arguments);
  END IF;

  RAISE EXCEPTION 'moment() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;

CREATE OR REPLACE FUNCTION central_moment(
  input ANYELEMENT,
  k integer,
  prov UUID = gate_one(),
  method text = NULL,
  arguments text = NULL)
  RETURNS DOUBLE PRECISION AS $$
DECLARE
  mu float8;
  total float8;
  i integer;
  raw_i float8;
  binom float8;
  -- iterative binomial coefficient C(k, i)
  k_double float8;
BEGIN
  IF pg_typeof(input) = 'random_variable'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    -- See variance() above: rv_moment handles the conditional/unconditional
    -- dispatch internally based on the resolved prov gate type.
    RETURN provsql.rv_moment(
      rv_conditioned_target((input::random_variable)::uuid), k, true,
      rv_conditioned_prov((input::random_variable)::uuid, prov));
  END IF;

  IF pg_typeof(input) = 'agg_token'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    IF k < 0 THEN
      RAISE EXCEPTION 'central_moment(): k must be non-negative (got %)', k;
    END IF;
    IF k = 0 THEN RETURN 1; END IF;
    IF k = 1 THEN RETURN 0; END IF;

    mu := agg_raw_moment(agg_conditioned_target(input::agg_token), 1,
                         rv_conditioned_prov(input::uuid, prov), method, arguments);
    IF mu IS NULL THEN RETURN NULL; END IF;
    -- mu may be ±Infinity for empty MIN / MAX with positive empty
    -- probability; central_moment is undefined in that case.
    IF mu = 'Infinity'::float8 OR mu = '-Infinity'::float8 THEN
      RETURN mu;
    END IF;

    total := 0;
    binom := 1;  -- C(k, 0)
    k_double := k;
    FOR i IN 0..k LOOP
      raw_i := agg_raw_moment(agg_conditioned_target(input::agg_token), i,
                              rv_conditioned_prov(input::uuid, prov), method, arguments);
      IF raw_i IS NULL THEN RETURN NULL; END IF;
      total := total + binom * power(-mu, k - i) * raw_i;
      -- C(k, i+1) = C(k, i) * (k - i) / (i + 1)
      IF i < k THEN
        binom := binom * (k_double - i) / (i + 1);
      END IF;
    END LOOP;
    RETURN total;
  END IF;

  -- Bernoulli event token (see moment()): with p = P(event),
  -- E[(X-p)^k] = (1-p)(-p)^k + p(1-p)^k; k = 0 -> 1, k = 1 -> 0.
  IF pg_typeof(input) = 'uuid'::regtype THEN
    IF input IS NULL OR k IS NULL THEN
      RETURN NULL;
    END IF;
    IF k < 0 THEN
      RAISE EXCEPTION 'central_moment(): k must be non-negative (got %)', k;
    END IF;
    IF k = 0 THEN RETURN 1; END IF;
    IF k = 1 THEN RETURN 0; END IF;
    mu := provsql.probability_evaluate(provsql.cond(input::uuid, prov),
                                       method, arguments);
    RETURN (1 - mu) * power(-mu, k) + mu * power(1 - mu, k);
  END IF;

  RAISE EXCEPTION 'central_moment() is not yet supported for input type %', pg_typeof(input);
END
$$ LANGUAGE plpgsql PARALLEL SAFE SET search_path=provsql SECURITY DEFINER;


SELECT reset_constants_cache();

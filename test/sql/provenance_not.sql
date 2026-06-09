\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Event negation: the prefix ! operator (function alias provenance_not) is the
-- complement of a Boolean provenance event -- sugar for monus(one, x), i.e.
-- Boolean NOT / probability 1-P(x).  It is an ordinary semiring expression (not
-- a measure-only marker), so it composes like any monus; a conditioned token is
-- refused as its child.  The motivating use is conditioning on the
-- non-occurrence of a UCQ violation event (a denial constraint): Q | ! W.

CREATE TABLE t(id int, p float);
INSERT INTO t VALUES (1,0.6),(2,0.5),(3,0.7);
SELECT add_provenance('t');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM t; END $$;

SET provsql.active = off;
DO $$
BEGIN
  PERFORM set_config('n.t1', (SELECT provsql FROM t WHERE id=1)::text, false);
  PERFORM set_config('n.t2', (SELECT provsql FROM t WHERE id=2)::text, false);
END $$;
RESET provsql.active;

-- !x = NOT x: P(!t1) = 1 - 0.6 = 0.4, and the prefix operator and the
-- provenance_not function agree.
SELECT abs(probability_evaluate(! current_setting('n.t1')::uuid) - 0.4) < 1e-9
         AS not_op_ok,
       probability_evaluate(! current_setting('n.t1')::uuid)
         = probability_evaluate(provenance_not(current_setting('n.t1')::uuid))
         AS op_eq_fn;

-- Double negation: !!t1 = t1  (probability back to 0.6).
SELECT abs(probability_evaluate(! ! current_setting('n.t1')::uuid) - 0.6) < 1e-9
         AS double_neg_ok;

-- Negation composes under times like any monus: P(t1 AND !t2) = 0.6*0.5 = 0.3.
SELECT abs(probability_evaluate(provenance_times(current_setting('n.t1')::uuid,
                                                 ! current_setting('n.t2')::uuid))
           - 0.3) < 1e-9 AS compose_ok;

-- Denial constraint: P(booking 1 present | no two overlapping same-room
-- bookings).  The violation event W is built by provenance aggregation over the
-- join (each joined row's provenance is a ⊗ b; GROUP BY () ORs them) -- no
-- hand-rolled gates -- captured into a GUC, then conditioned on its negation via
-- "| !".  Only pair (1,2) overlaps, so W = b1∧b2 (P=0.3) and the posterior is
-- 0.6·0.5/0.7 = 0.428571.
CREATE TABLE bookings(id int, room int, lo int, hi int, p float);
INSERT INTO bookings VALUES (1,7,10,20,0.6),(2,7,15,25,0.5),(3,7,30,40,0.7);
SELECT add_provenance('bookings');
DO $$ BEGIN PERFORM set_prob(provenance(), p) FROM bookings; END $$;

DO $$
DECLARE wtok uuid;
BEGIN
  SELECT provenance() INTO wtok
  FROM bookings a JOIN bookings b
    ON a.id<b.id AND a.room=b.room AND a.lo<b.hi AND b.lo<a.hi
  GROUP BY ();
  PERFORM set_config('n.w', wtok::text, false);
END $$;

SELECT abs(probability_evaluate((SELECT provenance() FROM bookings WHERE id=1)
                                | ! current_setting('n.w')::uuid)
           - 0.3/0.7) < 1e-9 AS denial_ok;

-- !(conditioned token) is refused: a conditioned token is terminal and may not
-- be buried under monus (conditioning has no semiring meaning under ⊖).
SELECT probability_evaluate(! cond(current_setting('n.t1')::uuid,
                                   current_setting('n.t2')::uuid));

SELECT remove_provenance('t');
SELECT remove_provenance('bookings');
DROP TABLE t;
DROP TABLE bookings;

\set ECHO none
\pset format unaligned

/* The subset semirings: the powerset Boolean algebra of the labels of an
   enum, whose difference reads negation exactly. */

/* Clearance: an employee is eligible unless an investigation is open, and
   an investigation is open unless resolved; investigations are confidential,
   resolutions secret. */
CREATE TYPE sub_level AS ENUM ('public','confidential','secret','top_secret');
CREATE TABLE sub_employee(name text, lvl sub_level);
INSERT INTO sub_employee VALUES ('Alice','public'),('Bob','public'),('Carol','public');
CREATE TABLE sub_investigation(name text, lvl sub_level);
INSERT INTO sub_investigation VALUES ('Alice','confidential'),('Bob','confidential');
CREATE TABLE sub_resolution(name text, lvl sub_level);
INSERT INTO sub_resolution VALUES ('Alice','secret');
SELECT add_provenance('sub_employee');
SELECT add_provenance('sub_investigation');
SELECT add_provenance('sub_resolution');
CREATE TABLE sub_clearance_map AS
  SELECT provsql AS provenance, lvl AS value FROM sub_employee
  UNION ALL SELECT provsql, lvl FROM sub_investigation
  UNION ALL SELECT provsql, lvl FROM sub_resolution;

/* Alice is visible at public (no investigation), not at confidential (an
   open one), again from secret (resolved); a single level (sr_minmax)
   cannot say so. */
CREATE TABLE result_clearance AS SELECT
  name,
  sr_clearance(provenance(),'sub_clearance_map','public'::sub_level) AS levels,
  visible_at(provenance(),'sub_clearance_map','public') AS at_public,
  visible_at(provenance(),'sub_clearance_map','confidential') AS at_confidential,
  (clearance_settling(provenance(),'sub_clearance_map','public'::sub_level)).*,
  sr_minmax(provenance(),'sub_clearance_map','public'::sub_level) AS minmax
FROM (
  SELECT name FROM sub_employee
  EXCEPT
  SELECT name FROM (SELECT name FROM sub_investigation
                    EXCEPT SELECT name FROM sub_resolution) open
) t;
SELECT remove_provenance('result_clearance');
SELECT * FROM result_clearance ORDER BY name;

/* An array is an arbitrary set of levels (compartments): secret no longer
   sees investigations, so Bob reappears there; sr_subset reads a single
   label as itself. */
CREATE TABLE sub_compartment_map AS
  SELECT provenance,
         CASE value WHEN 'public' THEN enum_range(NULL::sub_level)
                    WHEN 'confidential' THEN '{confidential,top_secret}'
                    ELSE '{secret,top_secret}' END AS value
  FROM sub_clearance_map;
CREATE TABLE result_compartment AS SELECT
  name,
  sr_clearance(provenance(),'sub_compartment_map','public'::sub_level) AS clearance,
  sr_subset(provenance(),'sub_clearance_map','public'::sub_level) AS subset
FROM (
  SELECT name FROM sub_employee
  EXCEPT
  SELECT name FROM (SELECT name FROM sub_investigation
                    EXCEPT SELECT name FROM sub_resolution) open
) t;
SELECT remove_provenance('result_compartment');
SELECT * FROM result_compartment ORDER BY name;

/* Consent: Alice's purchase of X is consented for billing only; the
   customers who did not buy X. */
CREATE TYPE sub_purpose AS ENUM ('billing','analytics','marketing');
CREATE TABLE sub_customer(id int, name text, consent sub_purpose[]);
INSERT INTO sub_customer VALUES
  (1,'Alice','{billing,analytics,marketing}'),
  (2,'Bob','{billing,analytics,marketing}'),
  (3,'Carol','{billing}');
CREATE TABLE sub_purchase(cust int, product text, consent sub_purpose[]);
INSERT INTO sub_purchase VALUES (1,'X','{billing}');
SELECT add_provenance('sub_customer');
SELECT add_provenance('sub_purchase');
CREATE TABLE sub_consent_map AS
  SELECT provsql AS provenance, consent AS value FROM sub_customer
  UNION ALL SELECT provsql, consent FROM sub_purchase;

/* Alice is returned for analytics and marketing only because her purchase
   is hidden from them: not over the whole database, so no purpose allows
   her. */
CREATE TABLE result_consent AS SELECT
  c.name,
  (sr_consent(provenance(),'sub_consent_map','billing'::sub_purpose)).*,
  consent_purposes(provenance(),'sub_consent_map','billing'::sub_purpose) AS allowed,
  consent_conflicts(provenance(),'sub_consent_map','billing'::sub_purpose) AS conflicts,
  consented_for(provenance(),'sub_consent_map','marketing') AS marketing
FROM sub_customer c
WHERE NOT EXISTS (SELECT * FROM sub_purchase p
                  WHERE p.cust = c.id AND p.product = 'X');
SELECT remove_provenance('result_consent');
SELECT * FROM result_consent ORDER BY name;

/* The enum argument is read for its type only: NULL::type gives the same
   results as a value of the enum, and a NULL token or mapping gives NULL. */
CREATE TABLE result_null_sample AS SELECT name,
       sr_clearance(provenance(),'sub_clearance_map',NULL::sub_level) AS clearance,
       sr_subset(provenance(),'sub_clearance_map',NULL::sub_level) AS subset,
       (clearance_settling(provenance(),'sub_clearance_map',NULL::sub_level)).*
FROM sub_employee;
SELECT remove_provenance('result_null_sample');
SELECT * FROM result_null_sample ORDER BY name;
DROP TABLE result_null_sample;
SELECT sr_subset(NULL,'sub_clearance_map',NULL::sub_level) IS NULL AS subset_token,
       sr_clearance(NULL,'sub_clearance_map',NULL::sub_level) IS NULL AS clearance_token,
       (clearance_settling(NULL,'sub_clearance_map',NULL::sub_level)).settles_at IS NULL
         AS settling_token,
       (sr_consent(NULL,'sub_consent_map',NULL::sub_purpose)).purposes IS NULL AS consent_token,
       consent_purposes(NULL,'sub_consent_map',NULL::sub_purpose) IS NULL AS purposes_token,
       consent_conflicts(NULL,'sub_consent_map',NULL::sub_purpose) IS NULL AS conflicts_token,
       sr_subset((SELECT provsql FROM sub_employee LIMIT 1),NULL,NULL::sub_level) IS NULL
         AS subset_mapping;

/* A label of another enum type is refused. */
SELECT visible_at(provenance(),'sub_clearance_map','marketing')
FROM sub_employee WHERE name = 'Alice';

DROP TABLE result_clearance, result_compartment, result_consent,
  sub_clearance_map, sub_compartment_map, sub_consent_map,
  sub_employee, sub_investigation, sub_resolution, sub_customer, sub_purchase;
DROP TYPE sub_level, sub_purpose;

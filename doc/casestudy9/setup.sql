-- Case Study 9: A Sales Forecast Dashboard
-- Setup script – load into a fresh PostgreSQL database:
--   psql -d mydb -f setup.sql

SET client_encoding = 'UTF8';

CREATE EXTENSION IF NOT EXISTS "uuid-ossp" WITH SCHEMA public;
CREATE EXTENSION IF NOT EXISTS provsql WITH SCHEMA public;

SET search_path TO public, provsql;
-- Make it the default of the database too, so that every later session
-- finds ProvSQL's functions without the provsql. prefix.
SELECT setup_search_path();

DROP TABLE IF EXISTS consent CASCADE;
DROP TABLE IF EXISTS deal CASCADE;
DROP TABLE IF EXISTS region CASCADE;
DROP TYPE IF EXISTS purpose CASCADE;

CREATE TABLE region (
    name   text PRIMARY KEY,
    target integer NOT NULL     -- revenue target, thousands of euros
);

CREATE TABLE deal (
    id       integer PRIMARY KEY,
    customer text NOT NULL,
    region   text NOT NULL REFERENCES region(name),
    quarter  text NOT NULL,
    amount   integer NOT NULL,          -- thousands of euros
    win_prob double precision NOT NULL  -- probability that the deal closes
);

INSERT INTO region (name, target) VALUES
    ('North', 150),
    ('South', 120),
    ('West',  100);

INSERT INTO deal (id, customer, region, quarter, amount, win_prob) VALUES
    ( 1, 'Arctis',    'North', 'Q1',  80, 0.9),
    ( 2, 'Borealis',  'North', 'Q1',  45, 0.5),
    ( 3, 'Fjordline', 'North', 'Q2', 120, 0.3),
    ( 4, 'Glacier',   'North', 'Q2',  30, 0.8),
    ( 5, 'Meridian',  'South', 'Q1',  60, 0.7),
    ( 6, 'Solstice',  'South', 'Q1',  25, 0.9),
    ( 7, 'Tropica',   'South', 'Q2',  90, 0.4),
    ( 8, 'Zenith',    'South', 'Q2',  40, 0.6),
    ( 9, 'Canyon',    'West',  'Q1',  70, 0.5),
    (10, 'Horizon',   'West',  'Q1',  35, 0.8),
    (11, 'Mesa',      'West',  'Q2',  55, 0.6),
    (12, 'Sierra',    'West',  'Q2', 150, 0.2);

-- The purposes each customer consented to the use of their data for
CREATE TYPE purpose AS ENUM ('forecasting', 'analytics', 'marketing');

CREATE TABLE consent (
    customer text PRIMARY KEY,
    purposes purpose[] NOT NULL
);

INSERT INTO consent (customer, purposes) VALUES
    ('Arctis',    '{forecasting,analytics,marketing}'),
    ('Borealis',  '{forecasting,analytics}'),
    ('Fjordline', '{forecasting}'),
    ('Glacier',   '{forecasting,analytics,marketing}'),
    ('Meridian',  '{forecasting,analytics,marketing}'),
    ('Solstice',  '{forecasting}'),
    ('Tropica',   '{forecasting,analytics,marketing}'),
    ('Zenith',    '{forecasting,analytics,marketing}'),
    ('Canyon',    '{forecasting,analytics,marketing}'),
    ('Horizon',   '{forecasting,analytics}'),
    ('Mesa',      '{forecasting,analytics,marketing}'),
    ('Sierra',    '{forecasting,analytics,marketing}');

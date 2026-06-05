-- Case Study 1: The Intelligence Agency
-- Setup script – load into a fresh PostgreSQL database:
--   psql -d mydb -f setup.sql

CREATE EXTENSION IF NOT EXISTS provsql CASCADE;

SET search_path TO public, provsql;

DROP TYPE IF EXISTS classification_level CASCADE;
CREATE TYPE classification_level AS ENUM (
    'unclassified', 'restricted', 'confidential', 'secret', 'top_secret',
    'unavailable'
);

DROP TABLE IF EXISTS personnel CASCADE;
CREATE TABLE personnel (
    id SERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    position TEXT NOT NULL,
    city TEXT NOT NULL,
    classification classification_level NOT NULL
);

TRUNCATE personnel;
INSERT INTO personnel (name, position, city, classification) VALUES
    ('Juma',   'Director',     'Nairobi', 'unclassified'),
    ('Paul',   'Janitor',      'Nairobi', 'restricted'),
    ('David',  'Analyst',      'Paris',   'confidential'),
    ('Ellen',  'Field agent',  'Beijing', 'secret'),
    ('Aaheli', 'Double agent', 'Paris',   'top_secret'),
    ('Nancy',  'HR',           'Paris',   'restricted'),
    ('Jing',   'Analyst',      'Beijing', 'secret');

#!/bin/sh
createdb tutorial
psql tutorial < setup.sql

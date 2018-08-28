#!/bin/sh
createdb stif
psql stif < demo_setup2.sql

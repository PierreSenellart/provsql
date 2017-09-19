#!/bin/sh
createdb demo
psql demo < demo_setup.sql

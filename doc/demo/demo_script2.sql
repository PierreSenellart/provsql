SET search_path TO public, provsql;
SET provsql.where_provenance=off;

DROP TABLE temp;
CREATE TABLE temp AS 
SELECT DISTINCT s2.stop_name, r2.route_long_name from stops s0,
                stop_times t1, stop_times t2, stops s1, stops s2, trips u2, routes r2
WHERE s1.stop_id=t1.stop_id AND s0.stop_name='GARE DE BAGNEUX' AND t1.trip_id=t2.trip_id AND s2.stop_id=t2.stop_id AND u2.trip_id=t2.trip_id
  AND r2.route_id=u2.route_id AND s1.parent_station=s0.stop_id AND t1.stop_sequence<t2.stop_sequence
ORDER BY route_long_name, stop_name;

SELECT * FROM temp;

SELECT *,boolean_sr(provenance(),'wheelchair') FROM temp WHERE route_long_name='B';

CREATE INDEX ON :`label2`;
CREATE INDEX ON :`label1`;
CREATE INDEX ON :`label3`;
CREATE INDEX ON :`label`(`prop`);
CREATE INDEX ON :`label2`(`prop`);
CREATE INDEX ON :__mg_vertex__(__mg_id__);
CREATE (:__mg_vertex__:`label` {__mg_id__: 0});
CREATE (:__mg_vertex__:`label` {__mg_id__: 1});
CREATE (:__mg_vertex__:`label` {__mg_id__: 2});
CREATE (:__mg_vertex__:`label` {__mg_id__: 3, `prop`: 1});
CREATE (:__mg_vertex__:`label` {__mg_id__: 4, `prop`: 2});
CREATE (:__mg_vertex__:`label` {__mg_id__: 5, `prop`: 3});
CREATE (:__mg_vertex__:`label2` {__mg_id__: 6, `prop2`: 1});
CREATE (:__mg_vertex__:`label3` {__mg_id__: 7});
DROP INDEX ON :__mg_vertex__(__mg_id__);
MATCH (u) REMOVE u:__mg_vertex__, u.__mg_id__;
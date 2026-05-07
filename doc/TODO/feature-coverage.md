# ProvSQL Feature Coverage

This file cross-references every user-facing feature documented under
`doc/source/user/` against the introductory tutorial and the five case
studies. It is meant as a maintenance aid: when adding a new feature,
extend the tables below; when extending the tutorial or a case study,
mark the additional cells.

Legend:

- **T**: Tutorial (`doc/source/user/tutorial.rst`, *Who Killed Daphine?*)
- **1**: Case study 1 (`casestudy1.rst`, *The Intelligence Agency*)
- **2**: Case study 2 (`casestudy2.rst`, *The Open Science Database*)
- **3**: Case study 3 (`casestudy3.rst`, *Île-de-France Public Transit*)
- **4**: Case study 4 (`casestudy4.rst`, *Government Ministers Over Time*)
- **5**: Case study 5 (`casestudy5.rst`, *The Wildlife Photo Archive*)
- `✓`: feature is exercised
- `(✓)`: feature is mentioned in passing but not actually executed
- empty cell: feature is not covered

## Setup and basics

| Feature                                  | T | 1 | 2 | 3 | 4 | 5 |
|------------------------------------------|---|---|---|---|---|---|
| `add_provenance`                         | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `remove_provenance`                      |   |   |   |   |   | ✓ |
| `provenance()` SELECT-list function      | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `create_provenance_mapping` (table)      | ✓ | ✓ | ✓ | ✓ |   |   |
| `create_provenance_mapping_view`         |   |   |   |   | ✓ |   |
| Hand-built mapping table                 |   |   |   |   |   | ✓ |
| `provsql.active` GUC                     |   |   |   |   |   |   |

## Supported SQL constructs

| Feature                                  | T | 1 | 2 | 3 | 4 | 5 |
|------------------------------------------|---|---|---|---|---|---|
| SELECT-FROM-WHERE / inner JOIN           | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Self-join                                | ✓ | ✓ |   | ✓ |   | ✓ |
| Subqueries in FROM / nested              |   | ✓ | ✓ |   |   | ✓ |
| GROUP BY                                 |   | ✓ | ✓ |   | ✓ | ✓ |
| SELECT DISTINCT                          | ✓ | ✓ |   | ✓ |   |   |
| EXCEPT (monus)                           | ✓ | ✓ |   |   |   | ✓ |
| UNION / UNION ALL                        |   |   |   |   |   |   |
| HAVING                                   |   |   | ✓ |   |   |   |
| VALUES                                   |   | ✓ |   |   |   | ✓ |
| CTE (WITH)                               |   |   |   |   |   | ✓ |
| LATERAL                                  |   |   |   |   |   |   |
| Window functions                         |   |   |   |   |   |   |
| FILTER clause on aggregates              |   |   |   |   |   |   |
| CREATE TABLE AS SELECT                   | ✓ |   |   | ✓ |   | ✓ |
| Provenance-bearing VIEW                  |   |   | ✓ |   | ✓ |   |
| INSERT … SELECT (provenance propagation) |   |   |   |   |   |   |

## Aggregation

| Feature                                  | T | 1 | 2 | 3 | 4 | 5 |
|------------------------------------------|---|---|---|---|---|---|
| COUNT / SUM / MIN / MAX / AVG            |   | ✓ | ✓ | ✓ | ✓ | ✓ |
| `string_agg` / `array_agg`               |   |   |   |   |   |   |
| `COUNT(DISTINCT …)`                      |   |   |   |   |   |   |
| Arithmetic / cast on aggregate result    |   |   | ✓ |   |   |   |
| `choose` aggregate                       |   |   |   |   |   |   |

## Circuit inspection

| Feature                                  | T | 1 | 2 | 3 | 4 | 5 |
|------------------------------------------|---|---|---|---|---|---|
| `get_gate_type`                          |   | ✓ |   |   |   |   |
| `get_children`                           |   | ✓ |   |   |   |   |
| `identify_token`                         |   | ✓ |   |   |   |   |
| `get_nb_gates`                           |   | ✓ |   |   |   |   |
| `get_infos`                              |   |   |   |   |   |   |
| `get_extra`                              |   |   |   |   |   |   |

## Semiring evaluation

| Feature                                  | T | 1 | 2 | 3 | 4 | 5 |
|------------------------------------------|---|---|---|---|---|---|
| `sr_boolean`                             |   |   |   | ✓ |   |   |
| `sr_boolexpr`                            |   | ✓ |   |   |   | ✓ |
| `sr_formula`                             | ✓ | ✓ | ✓ | ✓ |   | ✓ |
| `sr_counting`                            | ✓ |   | ✓ |   |   |   |
| `sr_why`                                 |   |   | ✓ |   |   |   |
| `sr_which`                               |   |   |   |   |   |   |
| `sr_tropical`                            |   |   |   |   |   |   |
| `sr_viterbi`                             |   |   |   |   |   |   |
| `sr_lukasiewicz`                         |   |   |   |   |   |   |
| `sr_temporal`                            |   |   |   |   | ✓ |   |
| `sr_interval_num`                        |   |   |   |   |   |   |
| `sr_interval_int`                       |   |   |   |   |   |   |
| Custom semiring via `provenance_evaluate`|   | ✓ | ✓ |   |   |   |
| `aggregation_evaluate`                   |   |   |   |   |   |   |

## Probabilities

| Feature                                  | T   | 1 | 2 | 3 | 4 | 5 |
|------------------------------------------|-----|---|---|---|---|---|
| `set_prob`                               | ✓   | ✓ | ✓ |   |   | ✓ |
| `get_prob`                               |     |   |   |   |   |   |
| `probability_evaluate` (default fallback)|     | ✓ | ✓ |   |   | ✓ |
| `'independent'` method                   |     |   |   |   |   |   |
| `'possible-worlds'` method               | ✓   | ✓ |   |   |   |   |
| `'monte-carlo'` method                   | (✓) | ✓ |   |   |   |   |
| `'tree-decomposition'` method            | (✓) | ✓ |   |   |   | ✓ |
| `'compilation'` (d4 / c2d / dsharp / minic2d) | (✓) | ✓ |   |   |   |   |
| `'weightmc'` method                      |     |   |   |   |   |   |
| `expected(COUNT/SUM/MIN/MAX)`            |     |   |   |   |   | ✓ |
| `repair_key` (block-independent, `mulinput`) |     |   |   |   |   | ✓ |

## Shapley and Banzhaf values

| Feature                                  | T | 1 | 2 | 3 | 4 | 5 |
|------------------------------------------|---|---|---|---|---|---|
| `shapley`                                |   |   | ✓ |   |   |   |
| `shapley_all_vars`                       |   |   | ✓ |   |   |   |
| `banzhaf`                                |   |   | ✓ |   |   |   |
| `banzhaf_all_vars`                       |   |   | ✓ |   |   |   |

## Where-provenance

| Feature                                  | T | 1 | 2 | 3 | 4 | 5 |
|------------------------------------------|---|---|---|---|---|---|
| `provsql.where_provenance` GUC           |   | ✓ | ✓ |   |   |   |
| `where_provenance(col)`                  |   | ✓ | ✓ |   |   |   |

## Data-modification tracking

| Feature                                  | T | 1 | 2 | 3 | 4 | 5 |
|------------------------------------------|---|---|---|---|---|---|
| `provsql.update_provenance` GUC          |   |   |   |   | ✓ |   |
| INSERT / UPDATE / DELETE tracked         |   |   |   |   | ✓ |   |
| `update_provenance` log table            |   |   |   |   | ✓ |   |
| `undo`                                   |   |   |   |   | ✓ |   |

## Temporal features

| Feature                                  | T | 1 | 2 | 3 | 4 | 5 |
|------------------------------------------|---|---|---|---|---|---|
| `union_tstzintervals`                    |   |   |   |   | ✓ |   |
| `timeslice`                              |   |   |   |   | ✓ |   |
| `timetravel`                             |   |   |   |   | ✓ |   |
| `history`                                |   |   |   |   | ✓ |   |
| `time_validity_view` extension           |   |   |   |   | ✓ |   |
| `get_valid_time`                         |   |   |   |   |   |   |

## Export and visualisation

| Feature                                  | T | 1 | 2 | 3 | 4 | 5 |
|------------------------------------------|---|---|---|---|---|---|
| `to_provxml`                             |   | ✓ |   |   |   |   |
| `view_circuit` (graph-easy)              |   | ✓ |   |   |   |   |
| `provsql.verbose_level`                  |   |   |   |   |   |   |

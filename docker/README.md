# ProvSQL

**(m-)semiring provenance and uncertainty management for PostgreSQL.**

ProvSQL is a PostgreSQL extension that transparently tracks the provenance of
query results as circuit tokens, enabling probabilities, Shapley/Banzhaf
values, and a wide range of semiring evaluations (Boolean, counting,
why/how/which, formula, tropical, Viterbi, Łukasiewicz, min-max…), plus
continuous random variables.

This image is a **ready-to-run demo**: PostgreSQL with ProvSQL preloaded, a
seeded `test` database, and **ProvSQL Studio** (the web UI) started for you.

## Quick start

```sh
docker run --rm -p 8000:8000 -p 5432:5432 inriavalda/provsql
```

- **ProvSQL Studio**: open <http://localhost:8000> – inspect provenance
  circuits, run semiring/probability evaluations, explore where-provenance.
- **psql**: connect to the `test` database as user `test` on port 5432, e.g.
  `psql -h localhost -p 5432 -U test test`.

No install needed for a pure-browser taste of ProvSQL – try **ProvSQL
Playground** instead (Studio compiled to WebAssembly, runs entirely
client-side): see <https://provsql.org/>.

## Tags

- `:latest` tracks the `master` branch.
- `:X.Y.Z` are tagged extension releases.

The bundled Studio version is pinned at image-build time, so a freshly tagged
extension image may carry a slightly older Studio (rebuilt later for a matched
pair).

## Documentation

- Project site: <https://provsql.org/>
- Documentation: <https://provsql.org/docs/>
- Source & issues: <https://github.com/PierreSenellart/provsql>

ProvSQL and ProvSQL Studio are licensed under the **MIT License**.

## Bundled third-party tools

For convenience the image also bundles optional external knowledge compilers,
model counters and renderers that ProvSQL can invoke as **separate
subprocesses** (never linked into the extension): **d4** and **dsharp**
(knowledge compilers), **ganak**, **sharpsat-td** and **weightmc** (weighted
model counters), **Graphviz** (`dot`), and **graph-easy**. Each is distributed
under **its own upstream license** (for instance Graphviz under EPL, graph-easy
under the Perl license; the compilers and counters under their respective
licenses, some research-only). The exact upstream sources, versions and build
steps are
recorded in
[`docker/Dockerfile`](https://github.com/PierreSenellart/provsql/blob/master/docker/Dockerfile);
refer to each project for its license terms.

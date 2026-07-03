"""Unit tests for the transparent-marker elision in circuit.py.

Pure functions (no database): the certificate / order-key parsers and the
``_elide_markers`` rewrite that turns elided gate_assumed and
gate_annotation wrappers into per-node badges -- including a node that carries
both a B and an IF badge.
"""
from __future__ import annotations

from provsql_studio import circuit


def test_parse_if_cert_reads_header_and_class_order():
    # C<kind> <nclasses> <root_class> <natoms> <maxarity> <class_topo_order...>
    cert = circuit._parse_if_cert("C1 3 0 4 2 0 1 2 0 0 1 1 2 2 0 0")
    assert cert == {
        "natoms": 4,
        "nclasses": 3,
        "root_class": 0,
        "class_order": [0, 1, 2],
    }


def test_parse_if_cert_rejects_non_recipe():
    assert circuit._parse_if_cert(None) is None
    assert circuit._parse_if_cert("K5 12 -1") is None
    assert circuit._parse_if_cert("Cgarbage") is None


def test_parse_if_key_reads_triple_and_guard():
    # K<factor> <root_len>:<root><sec_len>:<sec>; root/sec come back as value text.
    assert circuit._parse_if_key("K0 1:52:12") == {"root": "5", "sec": "12", "factor": 0}
    assert circuit._parse_if_key("K-1 1:52:12") == {"root": "5", "sec": "12", "factor": -1}
    assert circuit._parse_if_key("C1 1 0 1 1 0") is None
    assert circuit._parse_if_key("Kbad") is None


def _row(node, parent, pos, gtype, extra=None):
    return {"node": node, "parent": parent, "child_pos": pos,
            "gate_type": gtype, "extra": extra}


def test_elide_markers_stacks_b_and_if_on_one_node():
    # Scene:  assumed(wb) -> annotation/C(wc) -> times(t)
    #           t -> annotation/K(wk) -> input(i1)
    #           t -> input(i2)
    rows = [
        _row("wb", None, None, "assumed"),
        _row("wc", "wb", 0, "annotation", "C1 1 0 2 1 0"),
        _row("t", "wc", 0, "times"),
        _row("wk", "t", 0, "annotation", "K-1 1:52:12"),
        _row("i1", "wk", 0, "input"),
        _row("i2", "t", 1, "input"),
    ]
    out, new_root, markers = circuit._elide_markers(rows, "wb")

    # wrappers gone, root is the real times gate
    assert new_root == "t"
    assert {r["node"] for r in out} == {"t", "i1", "i2"}

    # the times gate carries BOTH badges, with the certificate detail
    assert markers["t"]["boolean_assumed"] is True
    assert markers["t"]["inversion_free"] is True
    assert markers["t"]["if_cert"] == {
        "natoms": 2, "nclasses": 1, "root_class": 0, "class_order": [0]}
    assert "if_key" not in markers["t"]

    # the wrapped input carries the IF order key (rank is added later, in _layout)
    assert markers["i1"]["inversion_free"] is True
    assert markers["i1"]["if_key"] == {"root": "5", "sec": "12", "factor": -1}
    assert "boolean_assumed" not in markers["i1"]
    # the unwrapped input carries nothing
    assert "i2" not in markers

    # edges: times is the scene root (parent None); both inputs hang off it,
    # i1 rewired through the elided K wrapper to its grandparent slot.
    parent_of = {r["node"]: (r["parent"], r["child_pos"]) for r in out}
    assert parent_of["t"] == (None, None)
    assert parent_of["i1"] == ("t", 0)
    assert parent_of["i2"] == ("t", 1)


def test_elide_markers_noop_without_wrappers():
    rows = [_row("t", None, None, "times"), _row("i", "t", 0, "input")]
    out, root, markers = circuit._elide_markers(rows, "t")
    assert out == rows and root == "t" and markers == {}


def test_gate_label_case_gate_glyph():
    # gate_case (the CASE-over-random_variable lowering: abs / ReLU / clamp)
    # renders with its own arrow glyph, so the guarded-selection node is
    # distinguishable in the circuit canvas.
    assert circuit._gate_label({"gate_type": "case"}) == "⇢"


def test_gate_label_transform_arith_opcodes():
    # The transform gate_arith opcodes (POW=7 / LN=8 / EXP=9) render their
    # operator glyph; POW is the caret, matching the `^` SQL operator.
    assert circuit._gate_label({"gate_type": "arith", "info1": "7"}) == "^"
    assert circuit._gate_label({"gate_type": "arith", "info1": "8"}) == "ln"
    assert circuit._gate_label({"gate_type": "arith", "info1": "9"}) == "exp"


def test_gate_label_percentile_arith_opcode():
    # PROVSQL_ARITH_PERCENTILE (10), the percentile_cont order-statistic
    # aggregate gate: the fraction from extra renders in the standard
    # percentile notation, with the bare "pct" glyph as the fallback for
    # a missing / unparseable extra.
    assert circuit._gate_label(
        {"gate_type": "arith", "info1": "10", "extra": "0.5"}) == "p50"
    assert circuit._gate_label(
        {"gate_type": "arith", "info1": "10", "extra": "0.375"}) == "p37.5"
    assert circuit._gate_label({"gate_type": "arith", "info1": "10"}) == "pct"
    assert circuit._gate_label(
        {"gate_type": "arith", "info1": "10", "extra": "bogus"}) == "pct"

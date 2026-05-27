"""End-to-end tests for the Tools panel (the external-tool registry).

Drive the live UI against the fresh PG database from conftest.py (whose
connecting role is a superuser, so the management controls are enabled)."""
from __future__ import annotations

from playwright.sync_api import Page, expect


def test_tools_panel_register_edit_unregister(page: Page, studio_url: str) -> None:
    page.goto(studio_url + "/where")
    page.locator("#tools-btn").click()
    expect(page.locator("#tools-panel")).to_be_visible()
    # the list is grouped by operation; a seeded compiler is under Compilation
    expect(page.locator("#tools-body")).to_contain_text("Compilation", timeout=8000)
    expect(page.locator("#tools-body")).to_contain_text("d4")

    # register a kcmcp endpoint tool through the form view
    page.locator("#tools-add-btn").click()
    expect(page.locator("#tools-form-view")).to_be_visible()
    page.locator("#tool-name").fill("e2e-kcmcp")
    page.locator("#tool-kind").select_option("kcmcp")
    expect(page.locator("#tool-exec-field")).to_be_hidden()        # cli-only hidden
    expect(page.locator("#tool-conn-field")).to_be_visible()
    page.locator("#tool-conn").select_option("endpoint")
    expect(page.locator("#tool-endpoint-field")).to_be_visible()
    page.locator("#tool-endpoint").fill("unix:/tmp/e2e.sock")
    page.locator("#tool-register-btn").click()                     # compile/ddnnf-nnf/nnf presets default

    expect(page.locator("#tools-list-view")).to_be_visible()
    row = page.locator(".wp-tools__row", has_text="e2e-kcmcp")
    expect(row).to_be_visible(timeout=8000)
    expect(row).to_contain_text("unix:/tmp/e2e.sock")
    expect(row.locator(".wp-tools__badge--kcmcp")).to_be_visible()

    # edit it via the per-row pencil: name is fixed, change the preference
    row.locator(".wp-tools__edit").click()
    expect(page.locator("#tools-form-view")).to_be_visible()
    expect(page.locator("#tool-name")).to_have_value("e2e-kcmcp")
    page.locator("#tool-pref").fill("42")
    page.locator("#tool-register-btn").click()
    expect(page.locator(".wp-tools__row", has_text="e2e-kcmcp")
               .locator(".wp-tools__pref")).to_have_value("42", timeout=8000)

    # unregister
    page.on("dialog", lambda d: d.accept())
    page.locator(".wp-tools__row", has_text="e2e-kcmcp").locator(".wp-tools__del").click()
    expect(page.locator(".wp-tools__row", has_text="e2e-kcmcp")).to_have_count(0, timeout=8000)


def test_tools_panel_registers_managed_server(page: Page, studio_url: str) -> None:
    page.goto(studio_url + "/where")
    page.locator("#tools-btn").click()
    page.locator("#tools-add-btn").click()
    page.locator("#tool-name").fill("e2e-managed")
    page.locator("#tool-kind").select_option("kcmcp")
    # Managed is the default connection -> no address field, endpoint = 'managed'
    expect(page.locator("#tool-endpoint-field")).to_be_hidden()
    page.locator("#tool-register-btn").click()
    row = page.locator(".wp-tools__row", has_text="e2e-managed")
    expect(row).to_be_visible(timeout=8000)
    expect(row).to_contain_text("managed")

    page.on("dialog", lambda d: d.accept())
    row.locator(".wp-tools__del").click()
    expect(page.locator(".wp-tools__row", has_text="e2e-managed")).to_have_count(0, timeout=8000)


def test_registered_compiler_appears_in_eval_strip(page: Page, studio_url: str) -> None:
    """Registering a compile tool must refresh the circuit-mode eval strip's
    compiler dropdown live (the /api/kc/tools cache is invalidated), not only
    after a page reload."""
    page.goto(studio_url + "/circuit")
    page.locator("#request").fill("SELECT name FROM personnel LIMIT 3;")
    page.locator("#run-btn").click()
    expect(page.locator("#result-count")).to_have_text("3", timeout=8000)
    page.locator("#result-body tr").first.locator("td").last.click()
    expect(page.locator("#eval-strip")).to_be_visible(timeout=8000)
    page.locator("#eval-semiring").select_option(value="probability")
    page.locator("#eval-method").select_option(value="compilation")
    compiler = page.locator("#eval-args-compiler")
    expect(compiler).to_be_visible()
    expect(compiler.locator("option[value='e2e-dropc']")).to_have_count(0)

    # register a kcmcp compile tool through the panel
    page.locator("#tools-btn").click()
    page.locator("#tools-add-btn").click()
    page.locator("#tool-name").fill("e2e-dropc")
    page.locator("#tool-kind").select_option("kcmcp")
    page.locator("#tool-conn").select_option("endpoint")
    page.locator("#tool-endpoint").fill("unix:/tmp/dropc.sock")
    page.locator("#tool-register-btn").click()
    page.locator("#tools-btn").click()   # close the panel

    # the dropdown now lists it, no reload needed
    expect(compiler.locator("option[value='e2e-dropc']")).to_have_count(1, timeout=8000)

    # cleanup
    page.locator("#tools-btn").click()
    page.on("dialog", lambda d: d.accept())
    page.locator(".wp-tools__row", has_text="e2e-dropc").locator(".wp-tools__del").click()


def test_nav_panels_mutually_exclusive(page: Page, studio_url: str) -> None:
    """Opening one top-nav popover (Schema / Config / Tools) closes the others."""
    page.goto(studio_url + "/where")
    page.locator("#schema-btn").click()
    expect(page.locator("#schema-panel")).to_be_visible()
    page.locator("#config-btn").click()
    expect(page.locator("#config-panel")).to_be_visible()
    expect(page.locator("#schema-panel")).to_be_hidden()
    page.locator("#tools-btn").click()
    expect(page.locator("#tools-panel")).to_be_visible()
    expect(page.locator("#config-panel")).to_be_hidden()
    expect(page.locator("#schema-panel")).to_be_hidden()

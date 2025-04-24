import os
import sys
from pathlib import Path
from docutils import nodes
from breathe.renderer.sphinxrenderer import SphinxRenderer
import xml.etree.ElementTree as ET

PROJECT_ROOT = Path(__file__).resolve().parent
REPO_ROOT_DIR = Path(__file__).parent.parent
sys.path.append(str(PROJECT_ROOT))

DOXYGEN_XML_DIR = "../doxygen-c/xml"

project = 'ProvSQL'
copyright = '2025, Pierre Senellart'
author = 'Pierre Senellart'

extensions = ['sphinx.ext.todo', 'breathe', 'sphinx.ext.graphviz']
templates_path = ['_templates']
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']

breathe_projects = {"C++": DOXYGEN_XML_DIR, "SQL": "../doxygen-sql/xml"}

html_theme = 'sphinx_rtd_theme'
html_theme_options = {
    'style_external_links': True,
    'collapse_navigation': False,
}
html_static_path = ['_static']

entries = {
    "file": {"dir": "files", "directive": "doxygenfile"},
}

breathe_file_src_url = "https://github.com/PierreSenellart/provsql/blob/master/src/{file_path}#L{line}"

# Helper to write an rst file


def write_rst(output_dir, name, title, directive_type, qualified_name):
    filepath = os.path.join(output_dir, f"{name}.rst")
    Path(os.path.dirname(filepath)).mkdir(exist_ok=True, parents=True)
    with open(filepath, "w") as f:
        f.write(f""".. _{name.lower()}:

{title}
{'=' * len(title)}
""")
        f.write(f"""
.. {directive_type}:: {qualified_name}
   :project: C++
   :allow-dot-graphs:\n""")
        if directive_type == "doxygenclass" or directive_type == "doxygenstruct":
            f.write("   :members:\n")
            f.write("   :protected-members:\n")
            f.write("   :undoc-members:\n")

    return filepath


# Top-level toctree entries
master_toc = []

for kind, meta in entries.items():
    dir_name = os.path.join("c", meta["dir"])
    rst_paths = []

    for filename in os.listdir(DOXYGEN_XML_DIR):
        if not filename.endswith(".xml"):
            continue

        xml_path = os.path.join(DOXYGEN_XML_DIR, filename)
        tree = ET.parse(xml_path)
        root = tree.getroot()
        compounddef = root.find("compounddef")
        if compounddef is None or compounddef.get("kind") != kind:
            continue
        if compounddef.get('language') != 'C++':
            continue

        compoundname = compounddef.find("compoundname").text
        location = compounddef.find("location").get('file')
        location = location.split("/src/")[-1]
        rst_path = write_rst(dir_name, location, location,
                             meta["directive"], compoundname)
        rel_path = f"{meta['dir']}/{location}"
        rst_paths.append(rel_path)

    # Write group index (e.g. classes.rst)
    group_index = os.path.join("c", f"{meta['dir']}.rst")
    with open(group_index, "w") as f:
        title = meta["dir"].capitalize()
        f.write(f"""{title}
{'=' * len(title)}

.. toctree::
   :maxdepth: 1

""")
        for p in sorted(rst_paths):
            f.write(f"   {p}\n")

    master_toc.append(f"{meta['dir']}")

def create_doxygen_target(self, node):
    try:
        loc = node.location
        file_path = loc.file[len(str(REPO_ROOT_DIR))+1:]
        url = breathe_file_src_url.format(file_path=file_path, line=loc.line)
        if file_path.find(".sql") != -1:
            url = url.replace("src/provsql.sql", "sql/provsql.sql")
        title = f"{file_path} on line {loc.line}"
        return [nodes.reference("", "", refuri=url, reftitle=title)]
    except:
        refid = self.get_refid(node.id)
        return self.target_handler.create_target(refid)

# Monkey patching this method:
SphinxRenderer.create_doxygen_target = create_doxygen_target

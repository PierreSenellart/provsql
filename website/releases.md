---
layout: single
title: "Releases"
permalink: /releases/
---

{% for release in site.data.releases %}
## Version {{ release.version }}

*Released {{ release.date | date: "%B %-d, %Y" }}*

{{ release.notes }}

{% if release.tag %}
[Download tarball](https://github.com/PierreSenellart/provsql/archive/refs/tags/{{ release.tag }}.tar.gz){:.btn .btn--primary}
[View on GitHub](https://github.com/PierreSenellart/provsql/releases/tag/{{ release.tag }}){:.btn .btn--inverse}

**Docker:**
```
docker pull inriavalda/provsql:{{ release.version }}
```
{% endif %}

---
{% endfor %}

For the full commit history, see the
[GitHub repository](https://github.com/PierreSenellart/provsql).

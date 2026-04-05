---
layout: single
title: "Demonstrations"
permalink: /demos/
---

Video demonstrations of ProvSQL in action.

{% for demo in site.data.demos %}
## {{ demo.title }}

*{{ demo.date | date: "%B %Y" }}*

{{ demo.description }}

{% if demo.paper_pdf %}<a href="{{ demo.paper_pdf }}" class="btn btn--small btn--inverse">Paper PDF</a>{% endif %}
{% if demo.paper_doi %}<a href="https://doi.org/{{ demo.paper_doi }}" class="btn btn--small btn--inverse">Paper DOI</a>{% endif %}

{% if demo.thumbnail %}
<a href="{{ demo.url }}" target="_blank" class="video-thumb-link">
  <img src="{{ demo.thumbnail }}" alt="{{ demo.title }}">
  <span class="video-thumb-play">&#9654;</span>
</a>
{% else %}
[Watch on YouTube]({{ demo.url }}){:target="_blank" .btn .btn--primary}
{% endif %}

---
{% endfor %}

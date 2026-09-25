---
layout: single
title: "Talks"
permalink: /talks/
toc: true
toc_label: "Contents"
toc_sticky: true
---

Slides of talks and tutorials about ProvSQL and the provenance theory
it builds on, by Pierre Senellart unless another speaker is named.
Slides of talks that present a specific paper are also linked from the
[publications](/publications/) page.

{% assign kinds = "outreach|tutorial|research" | split: "|" %}
{% for kind in kinds %}
{% case kind %}
{% when "outreach" %}
## For a general audience

Popularization talks: no prior knowledge of databases assumed.
{% when "tutorial" %}
## Tutorials and lectures

Introductions to database provenance and to ProvSQL, given at summer
schools and to non-specialist audiences.
{% when "research" %}
## Research talks

Talks on the design of ProvSQL and on the research it embodies.
{% endcase %}
{% assign talks = site.data.talks | where: "kind", kind %}
{% for talk in talks %}
### {{ talk.title }}

*{% if talk.speaker %}Presented by {{ talk.speaker }}. {% endif %}{{ talk.venue }}, {% if talk.month_only %}{{ talk.date | date: "%B %Y" }}{% else %}{{ talk.date | date: "%-d %B %Y" }}{% endif %}*{% if talk.language == "fr" %} – in French{% endif %}

{{ talk.description }}

<a href="{{ talk.slides }}" class="btn btn--small btn--primary">Slides</a>
{% if talk.video %}<a href="{{ talk.video }}" target="_blank" class="btn btn--small btn--inverse">Video</a>{% endif %}
{% if talk.paper_pdf %}<a href="{{ talk.paper_pdf }}" class="btn btn--small btn--inverse">Paper PDF</a>{% endif %}
{% if talk.paper_doi %}<a href="https://doi.org/{{ talk.paper_doi }}" class="btn btn--small btn--inverse">Paper DOI</a>{% endif %}
{% if talk.also %}
Also given at:
{% for other in talk.also %}
- {{ other.venue }}, {{ other.date | date: "%-d %B %Y" }} ([slides]({{ other.slides }}))
{%- endfor %}
{% endif %}
{% endfor %}
{% endfor %}

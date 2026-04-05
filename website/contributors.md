---
layout: single
title: "Contributors"
permalink: /contributors/
toc: true
toc_label: "Contents"
toc_sticky: true
---

ProvSQL has been developed primarily within the [Valda](https://team.inria.fr/valda/){:target="_blank"} research team, under the lead of [Pierre Senellart](https://pierre.senellart.com){:target="_blank"}.

<div class="institution-logos">
  <a href="https://www.ens.psl.eu/" target="_blank"><img src="/assets/images/institutions/ens-psl.png" alt="ENS-PSL"></a>
  <a href="https://www.cnrs.fr/" target="_blank"><img src="/assets/images/institutions/cnrs.svg" alt="CNRS"></a>
  <a href="https://www.inria.fr/" target="_blank"><img src="/assets/images/institutions/inria.svg" alt="Inria"></a>
</div>

## Team

{%- for member in site.data.team %}
- {% if member.url %}[**{{ member.name }}**]({{ member.url }}){:target="_blank"}{% else %}**{{ member.name }}**{% endif %}{% if member.affiliation %} – {% if member.affiliation_url %}[{{ member.affiliation }}]({{ member.affiliation_url }}){:target="_blank"}{% else %}{{ member.affiliation }}{% endif %}{% if member.location %}, {{ member.location }}{% endif %}{% endif %}
{%- endfor %}

*Contributions are welcome. See the
[GitHub repository](https://github.com/PierreSenellart/provsql) to get involved.*

## Grants

The development of ProvSQL has been supported by the following research grants.

{% for grant in site.data.grants %}
<div class="grant-entry">
  {% if grant.logo %}<div class="grant-logo"><a href="{{ grant.url }}" target="_blank"><img src="/assets/images/grants/{{ grant.logo }}" alt="{{ grant.name }}"></a></div>{% endif %}
  <h3 id="{{ grant.name | slugify }}">{% if grant.url %}<a href="{{ grant.url }}" target="_blank">{{ grant.name }}</a>{% else %}{{ grant.name }}{% endif %}</h3>
  <table class="grant-meta">
    <tr><th>Agency</th><td>{{ grant.agency }}{% if grant.agency_logos %}{% for al in grant.agency_logos %} <img src="/assets/images/agencies/{{ al.src }}" alt="{{ al.alt }}" class="agency-logo">{% endfor %}{% endif %}</td></tr>
    {% if grant.number != "" %}<tr><th>Grant number</th><td><code>{{ grant.number }}</code></td></tr>{% endif %}
    <tr><th>Period</th><td>{{ grant.start }}–{{ grant.end }}</td></tr>
    {% if grant.url %}<tr><th>Website</th><td><a href="{{ grant.url }}" target="_blank">{{ grant.url }}</a></td></tr>{% endif %}
  </table>
</div>

---
{% endfor %}

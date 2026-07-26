# Cardputer HTML Artifact Contract

Purpose: define a lightweight HTML authoring standard for pages that should remain readable inside the Cardputer BrowserApp text-first flow and in normal desktop/mobile browsers.

This is not a general web-rendering strategy. It is a contract for authoring/exporting pages that degrade well when JavaScript, CSS, images, and complex layout are absent or only partially supported.

## Use cases

- on-device status reports;
- saved troubleshooting guides;
- compact product explainers;
- offline reference pages copied to SD card;
- exported incident, test, or release notes intended for BrowserApp reading.

## Required properties

- single-file HTML preferred;
- no build step required to open the file;
- meaningful `<title>`;
- semantic structure: `header`, `main`, `section`, `nav`, `article`, `footer` where useful;
- correct heading order with a single `h1`;
- readable text without JavaScript;
- readable text when CSS is stripped or only partly supported;
- internal links and external links must stay useful in plain text form;
- no dependency on custom fonts, remote CSS, analytics, or third-party scripts.

## Text-first rules

- lead with the point in the first visible block;
- prefer one-column flow;
- keep paragraphs short and scannable;
- avoid dashboard-like dense grids unless the content still linearizes clearly;
- prefer plain lists and compact tables over decorative cards;
- every status/metric label must still make sense when color is absent;
- any image or diagram must have a text fallback or summary nearby.

## BrowserApp compatibility rules

- assume JavaScript may be unavailable or ignored;
- assume CSS selectors, spacing, and sticky/fixed behavior may degrade;
- avoid critical content hidden behind tabs, accordions, hover, or animation;
- avoid canvas-only, SVG-only, and image-only meaning;
- avoid wide multi-column comparison layouts unless they also serialize into a clear vertical reading order;
- prefer absolute or repository-local links that can be saved and reopened predictably.

## Recommended page shapes

- status report;
- incident report;
- feature explainer;
- troubleshooting guide;
- release or test note.

## Non-goals

- no SPA runtime;
- no heavy interactivity;
- no dependence on network-loaded assets;
- no assumption that arbitrary external sites will become readable on Cardputer by following this contract.

## Templates

- `templates/html/cardputer-status-report.html`
- `templates/html/cardputer-reference-page.html`

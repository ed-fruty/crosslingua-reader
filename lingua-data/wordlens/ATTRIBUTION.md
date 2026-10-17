# Word Lens data attributions

The `.lens` packs in this directory are converted from Readest's Word Lens JSON packs
(https://github.com/readest/readest, `apps/readest-app/data/wordlens`, served from
https://cdn.readest.com/wordlens) by `scripts/wordlens/update_packs.py`. Glosses are cleaned (first
senses only, markup, part-of-speech tags, stress marks and Arabic vowel signs removed).

Sources and licences (CC-BY-SA data stays CC-BY-SA in the converted packs):

- WikDict (bilingual glosses) - CC-BY-SA 3.0, derived from DBnary / Wiktionary. https://www.wikdict.com/
- kaikki.org wiktextract dump of the English Wiktionary (en-vi, en-hu, en-ar glosses) - CC-BY-SA 4.0.
  https://kaikki.org/dictionary/rawdata.html
- FrequencyWords (difficulty ranks, OpenSubtitles/OPUS) - CC-BY-SA 4.0. https://github.com/hermitdave/FrequencyWords
- ECDICT (en-zh and en-en glosses, ranks, English inflections) - MIT. https://github.com/skywind3000/ECDICT
- WordNet (en-en synonyms and hypernyms) - WordNet 3.0 licence. https://wordnet.princeton.edu/
- lemmatization-lists (source-language inflections) - https://github.com/michmech/lemmatization-lists

`word-lens-en-uk.lens` is derived by CrossLingua: its glosses are the en-ru pack's Russian
glosses machine-translated into Ukrainian (Google Translate), with en-ru's ranks and
inflections. It inherits en-ru's CC-BY-SA licence.

Each pack also carries its own attribution string (shown on the device):

- `word-lens-de-en.lens`: Glosses: WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0.
- `word-lens-en-ar.lens`: Glosses: Wiktionary (CC-BY-SA-4.0) via kaikki.org. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-4.0.
- `word-lens-en-de.lens`: Glosses: WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0.
- `word-lens-en-en.lens`: Word Lens data derived from Readest's Word Lens packs (https://github.com/readest/readest). See ATTRIBUTION.md for sources and licences.
- `word-lens-en-es.lens`: Glosses: WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0.
- `word-lens-en-fr.lens`: Glosses: WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0.
- `word-lens-en-hu.lens`: Glosses: Wiktionary (CC-BY-SA-4.0) via kaikki.org. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-4.0.
- `word-lens-en-pt.lens`: Glosses: WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0.
- `word-lens-en-ru.lens`: Glosses: WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0.
- `word-lens-en-uk.lens`: Glosses: machine-translated (Google Translate) from the Readest en-ru Word Lens pack, whose glosses come from WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0.
- `word-lens-en-vi.lens`: Glosses: Wiktionary (CC-BY-SA-4.0) via kaikki.org. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-4.0.
- `word-lens-en-zh.lens`: Word Lens data derived from Readest's Word Lens packs (https://github.com/readest/readest). See ATTRIBUTION.md for sources and licences.
- `word-lens-es-en.lens`: Glosses: WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0.
- `word-lens-fr-en.lens`: Glosses: WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0.
- `word-lens-it-en.lens`: Glosses: WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0.
- `word-lens-pt-en.lens`: Glosses: WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0.
- `word-lens-ru-en.lens`: Glosses: WikDict (CC-BY-SA-3.0), derived from DBnary/Wiktionary. Frequency: hermitdave/FrequencyWords from OpenSubtitles/OPUS (CC-BY-SA-4.0). License: CC-BY-SA-3.0.

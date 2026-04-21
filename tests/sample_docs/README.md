# Sample Documents for Extractor Tests

Real-world sample files used by the document extractor tests
(`PdfiumExtractor`, `DocxExtractor`, `XlsxExtractor` — see S2.3).

| File | Source | License |
|---|---|---|
| `sample.pdf` | [IRS Form W-9](https://www.irs.gov/pub/irs-pdf/fw9.pdf) | Public domain (U.S. federal government work, 17 U.S.C. § 105) |
| `sample.docx` | [Apache POI `test-data/document/SampleDoc.docx`](https://github.com/apache/poi/blob/trunk/test-data/document/SampleDoc.docx) | Apache License 2.0 |
| `sample.xlsx` | [Apache POI `test-data/spreadsheet/SampleSS.xlsx`](https://github.com/apache/poi/blob/trunk/test-data/spreadsheet/SampleSS.xlsx) | Apache License 2.0 |

Do not modify these files — tests assert on their exact contents
(page counts, heading text, cell values).

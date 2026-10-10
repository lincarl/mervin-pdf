# Microsoft Store submission

This worksheet contains the proposed English listing and the remaining
publication steps for Mervin PDF. It is not evidence that Microsoft has approved
or published the app.

## Product identity

| Field | Value |
| --- | --- |
| Product name | Mervin PDF |
| Publisher display name | Lincarl |
| Package identity name | `Lincarl.MervinPDF` |
| Package publisher | `CN=7444C5EF-8AD3-459D-A81A-9B90254807FC` |
| Store ID | `9PPM4Z5G1LD0` |
| Store link | <https://apps.microsoft.com/detail/9PPM4Z5G1LD0> |
| Device family | Desktop |
| Minimum platform | Windows 11, x64 |
| Recommended primary category | Productivity |
| Recommended secondary category | Utilities + tools |
| Proposed initial listing language | English (United States) |

## Short description

Read, measure, annotate, and organize PDFs on your computer. Mervin PDF brings
drawing measurements, local OCR, form filling, and page tools together without
an account or telemetry.

## Description

Mervin PDF is a desktop PDF reader for technical drawings and everyday
documents. Read, search, measure, annotate, fill forms, and organize pages with
document processing on your computer.

Measure distance, paths, area, perimeter, and angles. Use embedded drawing scales
or calibrate a page yourself, with snapping to drawing geometry. Save editable
measurements or export a flattened copy for sharing and printing.

Highlight text, add notes, and fill supported PDF forms. Rotate, delete, extract,
split, and merge pages. Inspect document encryption and permissions, and create
password-protected copies.

Keep several documents open with detachable tabs and multiple windows. Use
thumbnails, outlines, search, recent files, and saved reading positions to return
to your work.

OCR extracts text from a selected region using language models that you download
once. Recognition runs locally. Internet access is needed to download models or
open documents from web addresses. Microsoft Store manages application updates.

Mervin PDF is open source under the GNU Affero General Public License v3.0. The
application has no account requirement or telemetry. It supports form filling
and annotations, but does not edit original PDF text or images or add digital
signatures to documents.

## Product features

- Read and search PDFs with thumbnails, outlines, and flexible page layouts
- Measure drawings using embedded scales or manual calibration
- Snap measurements to drawing vertices and edges
- Save editable measurements or export a flattened copy
- Extract text from selected areas using local OCR
- Fill supported PDF forms and save your changes
- Highlight, underline, strike out text, and add notes
- Rotate, delete, extract, split, and merge pages
- Inspect and change PDF encryption and permissions
- Print with a live preview of forms, annotations, and measurements
- Work across detachable tabs and multiple windows
- Resume documents with recent files and saved view positions

Enter these as separate feature fields without the Markdown bullet characters.
Leave What's new in this version empty for the first submission.

## Links and license

- Project website and source code
  <https://github.com/lincarl/mervin-pdf>
- Privacy policy after this document is published on the main branch
  <https://github.com/lincarl/mervin-pdf/blob/main/docs/PRIVACY.md>
- License
  <https://github.com/lincarl/mervin-pdf/blob/main/LICENSE>
- Third-party notices
  <https://github.com/lincarl/mervin-pdf/blob/main/THIRD_PARTY_LICENSES.md>

Verify the privacy URL in a signed-out browser before submission. Partner Center
also accepts privacy policy text directly where that option is offered.

The public support URL is <https://github.com/lincarl/mervin-pdf/issues>.
GitHub Issues was enabled with the publisher's approval on 10 October 2026.
Do not copy the publisher's private sign-in email into the public listing.

For additional license terms, identify the application as AGPL-3.0 and retain the
bundled dependency notices. Keep corresponding source available for the exact
published binary and review distribution requirements in
[THIRD_PARTY_LICENSES.md](../THIRD_PARTY_LICENSES.md).

## Images

Use the existing Mervin icon for the package assets and a 300 by 300 pixel Store
icon. No new branding is needed.

Capture at least one real Windows desktop screenshot. Microsoft requires PNG
images at least 1366 by 768 pixels, no larger than 50 MB each. Four screenshots
are recommended. Suggested scenes are a measured drawing, an annotated
document, local OCR, and page or print tools.

Use synthetic documents that the project can redistribute. Do not use the
personal or third-party PDF corpus mentioned in [examples/README.md](../examples/README.md).
Keep account names, private paths, personal documents, and unrelated windows out
of screenshots. Do not substitute Linux screenshots or mockups for the Windows
application. Avoid added marketing text and decorative overlays.

Generate a redistributable drawing with
`python scripts/generate-store-sample.py <output-directory>/Birch-courtyard-studio.pdf`.
This original, fictional A3 floor plan includes room labels, furniture, dimensions
and an embedded 1:50 measurement scale. The generator uses only Python's standard
library. Open the generated PDF in the Windows application for screenshots. Keep
the generated PDF and screenshots outside the repository.

## Certification notes

Proposed explanation for `runFullTrust`:

> Mervin PDF is a C++/Qt desktop application. It needs full trust to open, edit,
> save, and print PDF files selected by the user and to run its desktop
> interface. Document processing and OCR run on the user's computer. It does
> not require administrator privileges, install services, or install drivers.

Proposed tester notes, to use after verifying the submitted package:

> No Mervin account is required. Open any local PDF from the application's Open
> action. The welcome screen lets the user choose interface languages and
> optionally download OCR language models. OCR requires a downloaded language
> model and an image region selected in a document. Downloading a model requires
> internet access. Reading, search, annotation, and measurement work with local
> files. Microsoft Store manages updates for this package. The PDF default-app
> action opens Windows Settings so the user can choose the default.

Only report completed checks. Before submission, verify installation, PDF file
activation, document save and printing, OCR downloads, update behavior,
uninstallation, and coexistence with an MSI installation on Windows.

## Account and release choices

Complete these in the authenticated Partner Center account.

1. Use the free price confirmed by the publisher on 10 October 2026. Choose
   markets and publication timing. Public availability after certification is
   the proposed publication timing.
2. Complete the IARC questionnaire based on the actual app. Do not assign an age
   rating manually or infer account-holder declarations. Microsoft shares the
   publisher display name and email address with IARC for rating administration.
3. Complete any account verification, tax, contact, or trader declarations the
   portal requires using the account holder's actual circumstances.
4. Upload the verified MSIX, complete the English listing and screenshots, and
   provide the privacy policy and restricted-capability explanation.
5. Submit for certification and monitor the result. Microsoft signs approved
   packages for Store distribution. This does not sign the standalone GitHub
   MSI installer or executable.

The first app submission and age-rating questionnaire must be created in Partner
Center before the submission API can create later submissions. Automation needs
an associated Microsoft Entra application with the Partner Center Manager role,
tenant ID, client ID, and a secret. The product's MSA app ID is not a substitute
for those credentials. Do not store a secret in the repository or mix portal
edits with an API-created submission.

## Microsoft documentation

- [Submission checklist](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/create-app-submission)
- [Store listing fields](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/add-and-edit-store-listing-info)
- [Screenshot and image requirements](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/screenshots-and-images)
- [Privacy and support fields](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/support-info)
- [Age-rating questionnaire](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/age-ratings)
- [Restricted capability approval](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/app-capability-declarations#restricted-capability-approval-process)
- [Submission API prerequisites](https://learn.microsoft.com/en-us/windows/uwp/monetize/create-and-manage-submissions-using-windows-store-services)

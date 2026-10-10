# Mervin PDF privacy policy

Updated 10 October 2026.

Mervin PDF processes PDFs on your computer. The application does not require a
Mervin account and does not send telemetry, usage analytics, or automatic crash
reports to its developer. It does not upload your documents for rendering,
search, OCR, measurement, form filling, annotation, or page and security
operations.

## Documents and local data

Mervin reads documents that you open and writes documents when you save or
export them. Documents can contain personal information, including form values
and annotations. New annotations include the author name from Settings. When
that setting is empty, Mervin uses your operating system user name. People who
receive those PDFs can read their contents and metadata.

Mervin stores settings, recent file paths and opening times, favorites, document
view positions, and session file paths in your user profile. These records let
you reopen documents and resume your work. Downloaded OCR language models are
also stored on your computer. The application does not send these records or
OCR results to its developer.

PDFs opened from a web address are downloaded to your Downloads folder, or to
the selected profile's downloads folder when using an isolated profile. They
remain there until you delete them. Saving a PDF to a network share or a folder
managed by a cloud synchronization service uses that location's existing access
and synchronization settings. Mervin does not provide its own cloud storage.

Printing sends document content to the printer and print service you select.
Copying text, images, or files puts the selection on the system clipboard.
Operating system features, such as clipboard synchronization, can handle that
data according to their own settings.

## Network connections

Mervin makes network requests for the following functions.

- Opening a web address downloads the requested document from that website.
  Opening a link in a PDF launches your default web browser.
- Opening Manage OCR languages loads the model catalog from GitHub. Downloading
  an OCR model retrieves it from the Tesseract project's GitHub repository.
  During first-run setup, continuing with Download OCR selected also downloads
  the models for your selected languages. OCR then runs on your computer.
- Microsoft Store installations receive application updates through Microsoft
  Store, subject to its update settings. They do not use Mervin's GitHub
  installer updater.
- Installations distributed outside Microsoft Store can check GitHub Releases
  for updates and download an installer. Automatic checks follow the application
  update setting. Manual checks follow your Check for Updates action.

Websites and download providers receive the request, your network IP address,
and request headers. The requested address identifies the document, language
model, or release being downloaded. A web address may itself contain personal
information. Mervin does not attach the contents of your local PDFs, their
passwords, or your recent-file history to these requests.

GitHub and Microsoft operate their services under their own privacy policies.
Other websites, browsers, storage services, and print services may also process
data independently of Mervin.

- [GitHub privacy statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement)
- [Microsoft privacy statement](https://privacy.microsoft.com/privacystatement)

## Your controls

You can use local documents without opening web addresses or downloading OCR
models. Clear Download OCR during first-run setup to skip those downloads. You
can remove downloaded models in Manage OCR languages.

Remove individual documents from the recent-file list using Remove from history.
This removes the history entry, not the PDF itself. You can change the recent
history retention and session restore settings in Settings. To remove all saved
application state, close every Mervin window and remove its user-profile data.
The app does not encrypt settings and history separately from the protection
provided by your operating system and storage. Access to these files depends on
your device's user accounts and file permissions.

Use your file manager to delete downloaded or saved PDFs. Removing the app does
not remove documents you saved outside its application data. Backups, synced
copies, and files you shared are controlled by their respective destinations.

Manage Store updates in Microsoft Store. For other installations, turn off
automatic updates in Mervin's Settings to stop scheduled GitHub update checks.

## About this policy

This policy covers Mervin PDF itself. The operating system, Microsoft Store,
GitHub, and other services you choose have their own data practices. The
[public project repository](https://github.com/lincarl/mervin-pdf) contains the
application source, documentation, and updates to this policy.

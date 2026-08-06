# Offline progress email draft

`generate_progress_email.py` turns an existing performance summary into a
plain-text RFC 5322 `.eml` draft. It is an outbox preparation tool only: it
does not import an SMTP client, open a network connection, or send a message.
The recipient address is required explicitly so a draft cannot silently target
an unknown mailbox.

Prefer the structured JSON output from `ra8p1_performance_summary.py`:

```powershell
python .\tools\generate_progress_email.py `
  --summary-json .\build\evidence\performance-summary.json `
  --to user@example.com `
  --output-eml .\build\evidence\progress-mail.eml `
  --output-txt .\build\evidence\progress-mail.txt
```

An existing Markdown report can also be wrapped. Bracketed
`[MEASURED]`, `[ESTIMATED]`, or `[MISSING]` markers are preferred. Older
reports that use plain `measured`/`estimated`/`missing` status words are also
accepted, with a weaker-provenance audit line; the generator preserves the
source text and does not reinterpret its numbers:

```powershell
python .\tools\generate_progress_email.py `
  --summary-md .\build\evidence\PROGRESS_CE29D2F3_PERFORMANCE.md `
  --to user@example.com `
  --output-eml .\build\evidence\progress-mail.eml
```

The `.eml` includes `X-Delivery-Status: DRAFT-NOT-SENT`. Supplying `--from`
and `--subject` changes only headers. Actual delivery remains a separate,
operator-approved step requiring the user's mail client or SMTP credentials.

JSON summaries are rendered with each timing and rate tagged as `MEASURED`,
`ESTIMATED`, or `MISSING`. A missing value is never replaced by a formula.
The summary's placeholder-model accuracy boundary and evidence sources are
included in the body.

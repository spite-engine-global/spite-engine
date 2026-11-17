---
name: spite-feedback
description: Prepare actionable upstream SPITE engine feedback when the user wants to report an engine issue or suggestion.
---

# SPITE feedback

Target https://github.com/spite-engine-global/spite-engine. Do not turn ordinary friction into automatic reports or repeatedly prompt the user. When feedback is requested, search existing issues for duplicates using available authenticated tools or `gh`; do not use a shared developer token. If search or authentication is unavailable, say so and preserve the draft locally.

Prepare an exact local draft with title, body and any proposed attachments. Separate user feedback, agent observations and inference. Include actual SDK and relevant module identities, reproducible steps, expected and actual results, and only relevant logs after redacting secrets and personal information. Identify uncertainties instead of inventing details.

Present the exact title, body, attachments and public destination to the user. Obtain explicit approval of all of them before uploading an attachment or posting anything; if any content or destination changes, obtain approval again. Until then keep everything local. Do not post merely because a draft was requested.

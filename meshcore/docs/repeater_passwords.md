# Repeater password controls

- Remember Password saves the entered password on Login, independently for
  each full 32-byte repeater public key. It survives reboot and does not log in
  automatically. Names and short key prefixes are not credential identifiers.
- On selection/re-entry a saved password stays outside the textarea. The field
  is empty and displays "Password saved"; Login retrieves it privately.
- Show Password is enabled only for newly typed text. It cannot reveal a saved
  password, and resets on login, selection changes, CLI entry and screen exit.
- Typing replaces the saved-password choice for that attempt. An empty edited
  field is an explicit empty password, not a fallback to the old saved secret.
- Unchecking Remember Password deletes that repeater's saved credential. A
  failed write/delete is reported rather than claiming the choice was saved.
- MeshCore accepts at most 15 password bytes; multibyte text is validated by
  byte length at Login. Empty passwords can be saved explicitly.

Credentials are versioned local LittleFS records under `/rptpw_<full-key>`.
Writes use a temporary file and rename, then readback verification. Unchanged
values avoid a flash write. These files are not encrypted or securely erased;
masking and non-reveal in the UI are not protection against physical flash
extraction. No credential is added to serial or web API responses.

Hardware checks: use two repeaters (including equal display names), remember
different passwords, reboot, select each and log in with an empty saved field.
Verify Show cannot expose either saved value. Retype a replacement, show/hide
it, log in, switch repeaters/tabs/CLI, and confirm masking resets. Uncheck
Remember, reboot and verify the credential is gone. Check portrait/landscape,
keyboard visibility, storage failure messages and an empty-password repeater.

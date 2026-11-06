# Repeater neighbours on the map

The logged-in repeater's Neighbours button opens Maps immediately. A matching
response highlights located repeaters in amber. The map requests `get lat` and
then `get lon` directly from the logged-in repeater after receiving the neighbour list, using MeshCore's reflected
two-character CLI prefixes to correlate replies. Only replies from the selected
full public key with the expected prefix and valid numeric coordinates are used.
Once both replies arrive, a teal origin pin and solid black lines connect that repeater
to its mapped neighbours. The CrowPanel's own fixed position is not used for links.
CLI position queries require an admin login on the repeater.

MeshCore neighbours responses return key prefixes and signal information.
Repeater locations are received separately in LoRa adverts. The map resolves
each neighbour against saved contacts, then a bounded RAM-only cache of received
repeater location adverts. Location capture runs before the automatic contact
admission filters, so declining to automatically save a repeater does not discard
its received location for the neighbour view. Capturing metadata does not add a
contact, unhide contacts, change routing or bypass the radio's advert validation.

The cache is allocated once in PSRAM, holds up to MAX_CONTACTS identities and is
cleared by Purge Data. It accepts valid nonzero locations from newer adverts,
preserves a known position if a later advert omits GPS, and rejects ambiguous
four-byte prefix matches. Known contacts with missing GPS use a full-key match.
No Internet service, SD node database, extra login or periodic scan is required.
After a reboot, neighbours absent from saved contacts need to be heard in a
location advert again. The neighbour list alone cannot supply missing coordinates.
Line labels display the SNR reported by the selected repeater; Names controls
also apply to advert-backed neighbour pins.

Unresolved contacts are reflected in the mapped/returned count. The
status also separates unknown contacts, missing/invalid GPS, ambiguous prefixes
and contacts that are not repeaters. Each explicit Neighbours request prints
one `[neighbour-map]` summary and a detailed `[neighbour-diag]` report to UART.
The bounded reply is retained while the overlay is open, and contact positions
are re-resolved by the existing map timer. Later adverts can therefore produce
links without pressing Neighbours again. Unchanged results do not recenter the
view or repeat the diagnostic log, and no extra radio queries are sent.

Pressing Neighbours requests a bounded 20-entry prefix/SNR snapshot for display
only. It does not import neighbours, create placeholder contacts, unhide saved
contacts, or prioritise neighbours in the repeater dropdown. Repeater lists
continue to use their normal advert/discovery handling. Two sequential
CLI position queries follow the neighbour reply per button press;
there is no periodic discovery or background position polling.

The retired importer previously saved names such as `Repeater_55687C00`. On boot,
contact loading skips only the exact generated name matching that contact's own
key prefix, with no received advert, no GPS, no flags and no saved route/path.
The cleaned list uses the existing transactional save, which keeps the previous
file as the normal `/contacts.bak` recovery copy. Advert-backed contacts, real
names, favourites/flags, coordinates and learned routes are retained. Modified
placeholders are intentionally kept rather than risking user data; the normal
Delete action can remove those individually. No chat history, passwords,
channels or identity are purged.

The view fits the located neighbours and the live repeater position at zoom 7-12.
Lines follow pan and zoom, use cached projections, and are clipped before LVGL
coordinate conversion. The close icon clears this overlay without deleting
contacts and cancels outstanding position queries. A missing repeater position
prevents lines but still shows located neighbour pins. Unset (0,0), invalid and
timed-out position replies have explicit status messages, rather than silently
falling back to the CrowPanel or an old contact location. Each request has an
estimated timeout with a minimum of 60 seconds.
Radio callbacks deposit a bounded reply; the existing map timer processes it
under the LVGL task, taking the mesh mutex while resolving contacts or sending
position commands. The longitude query follows the latitude reply to avoid
ambiguous replies and remote timestamp ordering problems.
Web API neighbour requests do not open a map.

## Serial Diagnostics

After flashing, open the 115200 baud monitor, log into the repeater, press
Neighbours once, and keep Maps open until `comparison complete` is printed.
With a large contact list, allow time for the batched inventory dump. Live
position requests may take up to their logged 60-120 second timeouts. No popup
or periodic radio discovery is added.

- `BEGIN`: selected repeater key and neighbour request tag.
- `request`: requested version, entry limit and key-prefix length, plus expected
  reply layout. Matching uses public-key bytes, never a repeater's display name.
- `reply ACCEPTED` / `IGNORED-tag` / `IGNORED-source` / `IGNORED-short`: received/expected tags, payload size, total
  neighbours, reported entries and safely usable entries.
- `inventory`: saved repeater count, how many have valid GPS, and the number
  of received location adverts retained in RAM.
- `compare`: each neighbour's four-byte key prefix, saved-contact and advert
  match counts, resolved name/GPS, metadata source and exclusion reason, followed
  by seconds since last heard and signed SNR. These fields help detect a
  mismatched remote reply layout instead of assuming every unknown ID is real.
  `screen_matches` and `screen_name` also compare the actual Repeater dropdown
  snapshot, including sticky entries that need not have a current saved GPS
  contact. The screen filter and 64-entry cap still apply to this UI snapshot;
  matching for map lines continues to inspect all saved contacts and GPS adverts.
- `raw`: the accepted decrypted neighbour response in bounded 16-byte hex rows,
  including the tag/header and encryption padding. Offsets allow offline checks
  of prefix length and entry boundaries; the data contains no shared/private key.
  Raw bytes are copied with the inventory and printed outside radio/cache locks.
- `detected`: saved repeaters and cached location adverts, with eight key bytes,
  name, GPS validity, advert timestamp and `in_neighbours=yes/no`. A repeater
  may appear in both sources; those are metadata records, not duplicate contacts.
- `origin query` / `origin reply`: tagged latitude/longitude commands, send
  success, received numeric text, parse failures, unexpected prefixes and timeouts.
- `origin READY`: live coordinates obtained from the logged-in repeater.
- `draw`: whether the origin exists, mapped neighbours, lines submitted to LVGL,
  viewport-clipped lines and coincident endpoints. `drawn` confirms a draw call,
  not that LCD pixels have been physically verified. A missing draw callback is
  also reported after three seconds.

For example, `saved_matches=0 advert_matches=0` means the neighbour key cannot
be located in either metadata source; `origin=MISSING` explains why no lines
can start even when neighbour positions are known. `coincident` means endpoints
share a screen pixel. All coordinates in the report are degrees, not the stored
integer microdegrees.

Inventory rows are copied to a bounded temporary PSRAM buffer, then printed
two at a time by a UI timer without holding mesh/cache locks. It is released
after the dump or when the overlay is cleared/destroyed. Diagnostic collection
does not import, rename, delete, unhide or save any contacts. Location data is
included: redact it before posting the logs publicly.

## Verification

- Compile `test/test_neighbour_map/test_neighbour_map.cpp` with C++17 to check
  truncated reply bounds, entry limits, distant line clipping and zero-length
  lines, coordinate validity, advert matching, stale updates, ambiguous prefixes,
  bounded cache replacement and purge clearing using static assertions.
- On hardware, log into a repeater with known neighbours, press Neighbours,
  verify the map opens, amber pins and black lines appear and remain aligned while panning
  and zooming. Check the mapped count against saved GPS contacts. Verify the
  teal origin matches the repeater's current `get lat` / `get lon` settings,
  even if the CrowPanel has no position or has a different saved position.
- Check unknown/no-GPS contacts, unset repeater position, guest login, no response,
  clearing before a reply, navigating away while waiting and repeated requests.
- Verify unrelated CLI messages, wrong senders and old query prefixes cannot
  overwrite the origin, and that errors/non-numeric replies never become (0,0).
- Confirm normal Maps navigation has no neighbour controls until requested,
  and that web requests do not change the visible screen.
- With automatic repeater saving off and Wi-Fi off, receive a valid repeater
  location advert for an identity absent from saved contacts. Verify its neighbour
  link appears automatically, while it remains absent from the saved-contact list.
- Verify Purge Data clears radio-derived locations and does not recapture them
  during that purged session.
- Confirm pressing Neighbours repeatedly does not add, rename, unhide or reorder
  saved repeaters. Unknown neighbours can remain unmapped until an advert arrives.
- Compile `test/test_neighbour_map/test_neighbour_contacts.cpp` to verify the
  narrow legacy-placeholder cleanup and preservation rules. Check cleanup on
  hardware with an existing imported list, then reboot again to check persistence.
- Test truncated replies, wrong source/tag, empty replies and no response without
  changing the saved-contact list or affecting unrelated radio operations.

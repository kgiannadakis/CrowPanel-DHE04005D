# Contributing

For this CrowPanel fork, follow the repository's
[contribution guide](../CONTRIBUTING.md) and the
[CrowPanel development guide](docs/crowpanel.md).

Keep protocol changes separate from board/UI changes, retain the existing
module style, and add tests appropriate to the affected behavior. Do not
reformat vendored libraries or unrelated upstream source. Pin dependency updates
explicitly and keep them separate from cleanup.

Changes intended for the official MeshCore project should follow the
[upstream contribution guide](https://github.com/meshcore-dev/MeshCore/blob/main/CONTRIBUTING.md)
and be discussed with that project's maintainers. This fork's tests and release
procedure are not the upstream multi-board build pipeline.

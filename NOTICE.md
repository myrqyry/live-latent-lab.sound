# Sound licensing and attribution

Sound's source is distributed under the GPL terms in [LICENSE](LICENSE).
The OBS plugin-support template contains a GPL-2.0-or-later notice.
Sound is an independent plugin for [OBS Studio](https://github.com/obsproject/obs-studio).
Developed in collaboration with a trusted AI colleague; this voluntary
acknowledgment describes the development process, not a warranty of quality.

External model weights and inference libraries are not licensed by this
repository's GPL. See [dependency and distribution inventory](docs/distribution.md)
for the boundary between source, the minimal test package, and separately
provisioned components. The small negative ONNX test fixture is derived
from Microsoft's ONNX Runtime distribution; see [fixture provenance](tests/fixtures/README.md).
The CI SDK recipe is reused from obs-ui-augment via Unscene; the workflow
calls the shared obs-plugin-ci project at a pinned revision.

No affiliation with OBS, model publishers or external speech services is implied.

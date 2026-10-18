# Releasing ShaderLab

A release is a tag `v<major>.<minor>.<patch>` on `main`. Pushing it runs
[`.github/workflows/release.yml`](../../.github/workflows/release.yml):

1. **CI** (`ci.yml`, called as a reusable workflow). A release is built only from
   a commit that passes it.
2. **Build** the MSIX for x64 and ARM64 (Release, sideload layout). The tag sets
   the package version (`v1.9.0` → `1.9.0.0`), and the step checks that the hub's
   `ShaderLabMcpBroker.exe` made it into the package (it is a hand-added payload
   and would otherwise be missed silently).
3. **Sign** both packages with Azure Artifact Signing, after approval in the
   `release` environment. Microsoft's own packages in `Dependencies\` are already
   signed and are left alone.
4. **Publish** a GitHub release with `ShaderLab-<version>-<platform>.zip`: the
   package, `Dependencies\` and `scripts/Install.ps1`.

`workflow_dispatch` builds the same packages (version `0.0.0.0`) without
publishing them.

## Signed and unsigned

Signing is on when the repository variable `MSIX_PUBLISHER` is set. The
package's `Publisher` must equal the signing certificate's subject exactly, so
the build writes `MSIX_PUBLISHER` into the manifest, and the sign job checks the
signer's subject against it. A signed package installs per user, with no
Developer Mode and no elevation, because Artifact Signing's public-trust
certificates chain to a root Windows trusts.

Without `MSIX_PUBLISHER` the release is unsigned. The manifest then carries the
unsigned-namespace OID (`CN=ShaderLab, OID.2.25.311729368913984317654407730594956997722=1`),
which `Add-AppxPackage -AllowUnsigned` requires, and installing needs Developer
Mode and an elevated prompt (an unsigned full-trust package installs only for
all users, `0x80073D2B` otherwise). `Install.ps1` picks the right path from the
package's signature.

The checked-in `Package.appxmanifest` keeps `CN=ShaderLab`, which local F5
deploys sign with the generated dev certificate. Signed releases therefore have
a different package family name from dev builds (it is derived from the
publisher), so the two install side by side, and the hub's AUMID differs.

## One-time setup

**Azure** (the Artifact Signing account and its Public Trust certificate profile,
after identity validation):

- An Entra app registration (`github-msix-signing`) with a federated credential
  per repository: issuer `https://token.actions.githubusercontent.com`, subject
  `repo:Spruill-1@62445444/ShaderLab@1193171108:environment:release`. The
  repositories use GitHub's immutable subject format (owner and repository IDs),
  so a renamed or re-created repository cannot borrow the trust.
- The **Artifact Signing Certificate Profile Signer** role for that app, on the
  certificate profile (or the account).

**GitHub**, on this repository:

- The `release` environment, with you as required reviewer and deployments
  limited to `v*` tags. The reviewer is the approval gate before anything is
  signed; the federated credential trusts only this environment.
- Repository variables (none is a secret; OIDC replaces stored credentials):

  | Variable | Value |
  |---|---|
  | `MSIX_PUBLISHER` | The certificate profile's subject, e.g. `CN=…, O=…, L=…, S=…, C=…` |
  | `AZURE_TENANT_ID` | Directory (tenant) ID |
  | `AZURE_CLIENT_ID` | The app registration's application (client) ID |
  | `AZURE_SUBSCRIPTION_ID` | The subscription holding the signing account |
  | `SIGNING_ENDPOINT` | The account's region endpoint, e.g. `https://eus.codesigning.azure.net/` |
  | `SIGNING_ACCOUNT` | The signing account's name |
  | `SIGNING_PROFILE` | The certificate profile's name |

## The private effects

[EffectGraph-Private](https://github.com/Spruill-1/EffectGraph-Private) publishes
ShaderLab with its effects as a related set: it downloads a ShaderLab release,
adds an optional package carrying its plugin, and signs the new packages with
the same profile. ShaderLab's package goes in unchanged, so the private release
can only wrap a **signed** ShaderLab release. ShaderLab needs nothing for it:
`Support/EffectPlugins.cpp` loads `Effects\` from every package it depends on,
optional packages included.

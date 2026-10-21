# Getting started

[English](GETTING-STARTED.md) · [한국어](GETTING-STARTED.ko.md)

[All guides](README.md) · [Features](FEATURES.md) · [Build from source](BUILDING.md)

## Install the client

You need a console that runs homebrew and a [CookingFoil server](https://github.com/jshsakura/oc-cookingfoil)
you have permission to access. Server setup is covered in its own repository.

1. Sign in to your server’s web page and download its `cookingfoil.nro`. The operator first supplies a client build in `data/client/` or through the admin upload page. See the [server guide](https://github.com/jshsakura/oc-cookingfoil#connecting-a-client).
2. Copy it to `sdmc:/switch/cookingfoil/cookingfoil.nro`.
3. Open it from the Homebrew Menu. For save access, use application mode by holding R while launching a game to open the Homebrew Menu.
4. Open **Settings → Add server**, enter your server address and the credentials it requires, and save.

A fresh installation starts with no servers. No third-party server settings are imported.

## Approve a device

If a server requires device approval, the console displays a QR code. Scan it on your phone,
sign in to that server and approve the device with an account allowed to do so. Once approved,
the console connects and keeps its access key in `cookingfoil.db`.

Approval belongs to each server. Removing approval on one server does not approve or revoke
the console on another. Device credentials are bound to the console; copying a database is not
a way to transfer approval to another device.

## Configure on a PC

You can also put `config.json` next to the NRO. The server's user administration page can provide
a client config; a signed-in browser can download one from `/api/client-config`. Passwords and
secret keys are not included in that generated config.

```json
{
  "servers": [
    {
      "title": "My server",
      "url": "https://files.example.com",
      "enabled": true
    }
  ],
  "language": "en",
  "installTarget": "sd"
}
```

| Key | Meaning |
|---|---|
| `servers[].title` | Display name; when omitted, the host name is used |
| `servers[].url` | HTTP or HTTPS server address |
| `servers[].enabled` | Whether to connect; defaults to `true` |
| `servers[].username`, `servers[].password` | HTTP Basic credentials when required |
| `servers[].cfClientId`, `servers[].cfClientSecret` | Cloudflare Access service token when required |
| `servers[].enrollToken` | Optional enrollment token provided by the server; removed after use |
| `language` | `ko`, `en`, `ja` or `zh` |
| `installTarget` | `sd` for the SD card or `nand` for console storage |

The [full example](config.example.json) includes a disabled backup server and optional authentication
fields. Replace the example addresses with your own. A server URL must not contain credentials,
a query or a fragment. A bare host name is treated as HTTPS.

After editing the file on a PC, use the reload action shown in Settings. The app saves server
edits back to `config.json`; interface choices made in the app are stored in the database and
can take precedence over the initial config values.

## Use several servers

Add servers from Settings or add entries to the config's `servers` array, up to 32.
Enabled servers contribute their catalogs. Items keep their server of origin; where several
servers offer the same item, the app lets you choose the download source. Server names and
colors help distinguish them when more than one is configured.

Homebrew, patches, retro and integrations depend on each server's advertised capabilities.
An absent optional menu can mean the server has no content for it yet.

## Files on the SD card

| Location | Purpose |
|---|---|
| `switch/cookingfoil/cookingfoil.nro` | App executable |
| `switch/cookingfoil/config.json` | Server configuration |
| `switch/cookingfoil/cookingfoil.db` | App state, device credentials, download queue and history |
| `switch/cookingfoil/log.txt` | App log for troubleshooting |
| `switch/cookingfoil/patches/` | Downloaded patch files and their placement ledger |
| `switch/oc-save-keeper/` | Local save backups and shared save-keeper settings |

Older `state.json` data is migrated to `cookingfoil.db` when the app can bind and save it.
Keep the database and config when replacing the NRO for an update.

## Check a connection

- Confirm the server address works from another device on the same network.
- If approval is pending, finish the approval for that server on your phone.
- If the server requires HTTP Basic or Cloudflare Access credentials, supply the matching fields.
- If save access fails in applet mode, relaunch in application mode as the app requests.
- If an optional feature is missing, check the server's content and settings before changing the client.

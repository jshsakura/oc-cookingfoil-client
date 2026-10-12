cacert.pem is the Mozilla CA bundle distributed by curl:
https://curl.se/docs/caextract.html
Downloaded from https://curl.se/ca/cacert.pem on 2026-10-06.
The certificates are extracted from Mozilla's NSS certdata.txt, which is
available under the Mozilla Public License 2.0 (https://mozilla.org/MPL/2.0/).
The devkitPro libcurl ships without a default CA bundle. API and image requests use
romfs:/cacert.pem and retain certificate and hostname verification.

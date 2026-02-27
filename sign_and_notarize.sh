#!/bin/bash

# =============================================================================
# macOS Code Signing, Notarization & Stapling - Reference Script
# =============================================================================
#
# PREREQUISITES:
#   1. Apple Developer account ($99/year)
#   2. "Developer ID Application" certificate installed in Keychain
#   3. App-specific password generated at https://appleid.apple.com
#   4. Xcode command line tools installed
#
# SETUP:
#   1. Create .env.signing (DO NOT commit to git) with:
#        APPLE_ID="your-apple-id@example.com"
#        APPLE_APP_PASSWORD="xxxx-xxxx-xxxx-xxxx"
#   2. Update CERTIFICATE_NAME and TEAM_ID below with your values
#
# USAGE:
#   ./sign_and_notarize.sh build_mac/Release/SkeletonVulkan.plugin
#
# =============================================================================

set -e

# ---- Configuration (update these with your values) ----
CERTIFICATE_NAME="Developer ID Application: Your Company (XXXXXXXXXX)"
TEAM_ID="XXXXXXXXXX"

# Load credentials from .env.signing
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENV_FILE="$SCRIPT_DIR/.env.signing"

if [ ! -f "$ENV_FILE" ]; then
    echo "[ERROR] .env.signing not found. Create it with:"
    echo '  APPLE_ID="your-apple-id@example.com"'
    echo '  APPLE_APP_PASSWORD="xxxx-xxxx-xxxx-xxxx"'
    exit 1
fi

source "$ENV_FILE"

if [ -z "$APPLE_ID" ] || [ -z "$APPLE_APP_PASSWORD" ]; then
    echo "[ERROR] APPLE_ID and APPLE_APP_PASSWORD must be set in .env.signing"
    exit 1
fi

# ---- Input validation ----
PLUGIN_PATH="$1"

if [ -z "$PLUGIN_PATH" ]; then
    echo "Usage: $0 <path-to-plugin.plugin>"
    echo "  e.g. $0 build_mac/Release/SkeletonVulkan.plugin"
    exit 1
fi

if [ ! -d "$PLUGIN_PATH" ]; then
    echo "[ERROR] Plugin not found: $PLUGIN_PATH"
    exit 1
fi

PLUGIN_NAME=$(basename "$PLUGIN_PATH" .plugin)

echo "========================================"
echo "Sign & Notarize: $PLUGIN_NAME"
echo "========================================"
echo ""

# ---- Step 1: Sign embedded libraries first (inside-out signing) ----
echo "[1/4] Signing embedded libraries..."

# Sign any embedded dylibs (e.g., Vulkan loader, MoltenVK)
find "$PLUGIN_PATH/Contents/MacOS" -name "*.dylib" 2>/dev/null | while read dylib; do
    echo "  Signing: $(basename "$dylib")"
    codesign --force --sign "$CERTIFICATE_NAME" \
        --timestamp \
        --options runtime \
        "$dylib"
done

echo ""

# ---- Step 2: Sign the plugin bundle ----
echo "[2/4] Signing plugin bundle..."

codesign --force --sign "$CERTIFICATE_NAME" \
    --timestamp \
    --options runtime \
    --deep \
    "$PLUGIN_PATH"

# Verify signature
codesign --verify --verbose "$PLUGIN_PATH"
echo "  Signature verified."
echo ""

# ---- Step 3: Notarize ----
echo "[3/4] Notarizing (this may take a few minutes)..."

# Create a ZIP for notarization submission
ZIP_PATH="/tmp/${PLUGIN_NAME}_notarize.zip"
ditto -c -k --keepParent "$PLUGIN_PATH" "$ZIP_PATH"

# Submit for notarization using notarytool (Xcode 13+)
xcrun notarytool submit "$ZIP_PATH" \
    --apple-id "$APPLE_ID" \
    --password "$APPLE_APP_PASSWORD" \
    --team-id "$TEAM_ID" \
    --wait

if [ $? -ne 0 ]; then
    echo ""
    echo "[ERROR] Notarization failed!"
    echo "  Run with --verbose for details, or check:"
    echo "  xcrun notarytool log <submission-id> --apple-id $APPLE_ID --password ... --team-id $TEAM_ID"
    rm -f "$ZIP_PATH"
    exit 1
fi

rm -f "$ZIP_PATH"
echo ""

# ---- Step 4: Staple the notarization ticket ----
echo "[4/4] Stapling notarization ticket..."

xcrun stapler staple "$PLUGIN_PATH"

echo ""
echo "========================================"
echo "DONE"
echo "========================================"
echo ""
echo "Plugin signed, notarized, and stapled: $PLUGIN_PATH"
echo ""
echo "Verify with:"
echo "  spctl --assess --type execute --verbose \"$PLUGIN_PATH\""
echo "  stapler validate \"$PLUGIN_PATH\""
echo ""

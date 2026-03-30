#!/bin/bash
# Generate signing keys for Virtual Camera APEX
# For development only - use proper key management for production!

set -e

APEX_NAME="com.android.hardware.camera.provider.virtual"

echo "Generating APEX signing keys for ${APEX_NAME}..."

# Generate RSA key pair for APK signing
openssl genrsa -out ${APEX_NAME}.pem 4096

# Generate self-signed certificate
openssl req -new -x509 -key ${APEX_NAME}.pem \
    -out ${APEX_NAME}.x509.pem \
    -days 10000 \
    -subj "/CN=${APEX_NAME}"

# Convert to PKCS#8 format (required by Android build)
openssl pkcs8 -topk8 -inform PEM -outform DER \
    -in ${APEX_NAME}.pem \
    -out ${APEX_NAME}.pk8 \
    -nocrypt

# Generate AVB key (for verified boot)
# This requires avbtool from AOSP
if command -v avbtool &> /dev/null; then
    avbtool extract_public_key --key ${APEX_NAME}.pem --output ${APEX_NAME}.avbpubkey
    echo "AVB public key generated."
else
    echo "Warning: avbtool not found. Run from AOSP environment to generate AVB key:"
    echo "  avbtool extract_public_key --key ${APEX_NAME}.pem --output ${APEX_NAME}.avbpubkey"
fi

echo ""
echo "Generated files:"
ls -la ${APEX_NAME}.*
echo ""
echo "Done! Copy these to the apex/ directory."

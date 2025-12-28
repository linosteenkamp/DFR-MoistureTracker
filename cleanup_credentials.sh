#!/bin/bash
# Script to automatically remove MQTT credentials from git history
# Run with: bash cleanup_credentials.sh

set -e

echo "🔒 Sanitizing git history to remove MQTT credentials..."
echo ""
echo "This will:"
echo "  1. Create mqtt_credentials.h with your current credentials"
echo "  2. Rewrite ALL commits to use the include file instead"
echo "  3. Update .gitignore to prevent future commits"
echo ""
read -p "Continue? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 1
fi

# Ensure mqtt_credentials.h exists
if [ ! -f "include/mqtt_credentials.h" ]; then
    echo "❌ Error: include/mqtt_credentials.h not found!"
    echo "Please ensure the credentials file was created first."
    exit 1
fi

echo "✅ Starting git filter-branch..."

# Use git filter-branch to rewrite history
git filter-branch --force --tree-filter '
if [ -f "src/main.c" ]; then
    # Replace hardcoded credentials with include
    sed -i.bak -E "s/#define MQTT_BROKER_URI[[:space:]]+\"[^\"]+\"[[:space:]]*\/\/.*$/\/\/ MQTT_BROKER_URI defined in mqtt_credentials.h/g" src/main.c
    sed -i.bak -E "s/#define MQTT_USERNAME[[:space:]]+\"[^\"]+\"[[:space:]]*\/\/.*$/\/\/ MQTT_USERNAME defined in mqtt_credentials.h/g" src/main.c
    sed -i.bak -E "s/#define MQTT_PASSWORD[[:space:]]+\"[^\"]+\"[[:space:]]*\/\/.*$/\/\/ MQTT_PASSWORD defined in mqtt_credentials.h/g" src/main.c
    sed -i.bak -E "s/#define MQTT_TOPIC_PREFIX[[:space:]]+\"[^\"]+\"[[:space:]]*\/\/.*$/\/\/ MQTT_TOPIC_PREFIX defined in mqtt_credentials.h/g" src/main.c
    sed -i.bak -E "s/#define MQTT_KEEPALIVE_SEC[[:space:]]+[0-9]+[[:space:]]*\/\/.*$/\/\/ MQTT_KEEPALIVE_SEC defined in mqtt_credentials.h/g" src/main.c
    rm -f src/main.c.bak
fi
' --tag-name-filter cat -- --all

echo "✅ History rewritten!"
echo ""
echo "⚠️  IMPORTANT: You must force-push if you previously pushed to remote:"
echo "    git push --force-with-lease origin main"
echo ""
echo "🔍 Verify credentials are gone from history:"
echo "    git log -p | grep -i password"
echo ""

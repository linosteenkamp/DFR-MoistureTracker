#!/bin/bash
# Fresh start: Remove git history and start clean

echo "🔄 Creating fresh git repository without credential history..."
echo ""
read -p "This will DELETE all git history. Continue? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 1
fi

# Backup current .git in case you change your mind
echo "📦 Backing up current .git to .git.backup..."
mv .git .git.backup

# Initialize fresh repository
echo "✨ Creating new repository..."
git init
git add .
git commit -m "Initial commit - DFR-MoistureTracker with secure credentials"

echo ""
echo "✅ Done! Clean repository created."
echo ""
echo "📝 Summary of what happened:"
echo "   - Old git history backed up to .git.backup"
echo "   - New repository initialized with current code"
echo "   - MQTT credentials now in mqtt_credentials.h (gitignored)"
echo "   - Safe to push to GitHub"
echo ""
echo "🚀 To push to GitHub:"
echo "   git remote add origin <your-github-url>"
echo "   git branch -M main"
echo "   git push -u origin main"
echo ""
echo "💾 To restore old history (if needed):"
echo "   rm -rf .git && mv .git.backup .git"
echo ""

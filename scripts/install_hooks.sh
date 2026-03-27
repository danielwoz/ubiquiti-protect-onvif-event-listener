#!/bin/bash
set -e
cd "$BUILD_WORKSPACE_DIRECTORY"
git config core.hooksPath .githooks
echo "Git hooks installed. Pre-push hook will run lint, tests, and PGO bench."

#!/bin/bash

# Auto Git Merge Script

# Set the repository and branch to merge
REPO_URL="https://github.com/abdulrahman305/vim.git"
BRANCH="main"

# Clone the repository
git clone $REPO_URL
cd vim

# Fetch the latest changes
git fetch origin

# Checkout the branch to merge
git checkout $BRANCH

# Merge the changes
git merge origin/$BRANCH

# Push the changes
git push origin $BRANCH

# Clean up
cd ..
rm -rf vim

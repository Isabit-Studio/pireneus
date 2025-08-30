#!/bin/sh
set -e

CMAKE_BIN=$(sh scripts/sh/cmake/cmake-download.sh --print-bin)


TASK_DIR=$(dirname "$TASK_BIN")
CMAKE_DIR=$(dirname "$CMAKE_BIN")

SHELL_NAME=$(basename "$SHELL")
RC_FILE=""
PROMPT_LINE=""

case "$SHELL_NAME" in
  bash)
    RC_FILE=".bashrc"
    PROMPT_LINE='export PS1="[pireneus] \w \$ "'
    ;;
  zsh)
    RC_FILE=".zshrc"
    PROMPT_LINE='export PS1="%F{blue}[pireneus]%f %~ %# "'
    ;;
  *)
    RC_FILE=".projectrc"
    PROMPT_LINE='export PS1="[pireneus] \w \$ "'
    ;;
esac

cat > "$RC_FILE" <<EOF
# Auto-generated $RC_FILE for pireneus

# Prompt customization
$PROMPT_LINE

# Aliases
alias task="$TASK_BIN"
alias cmake="$CMAKE_BIN"

# Optional PATH extension
export PATH="$CMAKE_DIR:\$PATH"
EOF

echo "✅ $RC_FILE generated for $SHELL_NAME."


CYAN='\033[36m' 
RESET='\033[0m'

echo -e "${CYAN}👉 Run 'source $RC_FILE' to activate.${RESET}"

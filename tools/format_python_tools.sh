#!/bin/bash

set -e

VENV_DIR="pyformat"
SCRIPTPATH=$(realpath "$(dirname $0)")
. $SCRIPTPATH/shell_utils.sh
cd "$SCRIPTPATH"


source_venv "$VENV_DIR"
echo "Installing requirements..."
pip3 install -r requirements.txt

echo "Running Python format check..."
python3 format_python_tools.py $1

echo "Running Python3 flake8 check..."
python3 -m flake8 . --exclude=*/venv/* --count --select=E901,E999,F821,F822,F823 --show-source --statistics

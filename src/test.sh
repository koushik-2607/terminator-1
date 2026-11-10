#!/bin/bash

echo "================================"
echo "   Terminator Bash Test"
echo "================================"

echo "Hello! Bash script is running."

echo ""
echo "Current User: $(whoami)"
echo "Current Directory: $(pwd)"
echo "Current Date: $(date)"

echo ""
echo "Testing basic calculations..."

a=10
b=20
sum=$((a + b))

echo "Sum of $a + $b = $sum"

echo ""
echo "Bash script executed successfully!"

echo "================================"
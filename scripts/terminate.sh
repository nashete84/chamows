#!/bin/bash
# Deletes a Chamows machine so it stops costing money. The AMI is kept.
#
#   ./scripts/terminate.sh i-0123456789abcdef0 [--region eu-west-1]
set -euo pipefail

INSTANCE_ID="${1:-}"
REGION="${AWS_REGION:-eu-west-1}"
[ "${2:-}" = "--region" ] && REGION="${3:-$REGION}"
[[ "$INSTANCE_ID" =~ ^i-[0-9a-f]+$ ]] || { echo "uso: $0 i-XXXXXXXX [--region REGION]" >&2; exit 1; }

NAME=$(aws ec2 describe-instances --region "$REGION" --instance-ids "$INSTANCE_ID" \
       --query "Reservations[0].Instances[0].Tags[?Key=='Project'].Value | [0]" --output text)
[ "$NAME" = "chamows" ] || { echo "La instancia $INSTANCE_ID no es de Chamows; no la toco." >&2; exit 1; }

read -r -p "¿Borrar $INSTANCE_ID y todo su disco? Escribe 'si': " ANSWER
[ "$ANSWER" = "si" ] || { echo "Cancelado."; exit 0; }

aws ec2 terminate-instances --region "$REGION" --instance-ids "$INSTANCE_ID" >/dev/null
echo "Borrando $INSTANCE_ID…"
aws ec2 wait instance-terminated --region "$REGION" --instance-ids "$INSTANCE_ID"
echo "Listo."

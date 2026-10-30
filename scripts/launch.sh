#!/bin/bash
# Launches a Chamows machine on AWS from the newest Chamows AMI.
#
#   ./scripts/launch.sh [--region eu-west-1] [--type t3.large] [--ami ami-...] [--key mi-clave]
#
# Needs the AWS CLI configured with your account (`aws configure`).
# Only your current public IP can reach the machine.
set -euo pipefail

REGION="${AWS_REGION:-eu-west-1}"
TYPE="t3.large"
AMI=""
KEY_NAME=""
SG_NAME="chamows-sg"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --region) REGION="$2"; shift 2 ;;
    --type)   TYPE="$2"; shift 2 ;;
    --ami)    AMI="$2"; shift 2 ;;
    --key)    KEY_NAME="$2"; shift 2 ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) die "opción desconocida: $1" ;;
  esac
done

command -v aws >/dev/null || die "instala la AWS CLI: https://aws.amazon.com/cli/"
command -v openssl >/dev/null || die "falta openssl"
export AWS_DEFAULT_REGION="$REGION"
aws sts get-caller-identity >/dev/null || die "la AWS CLI no tiene credenciales (ejecuta 'aws configure')"

if [ -z "$AMI" ]; then
  AMI=$(aws ec2 describe-images --owners self --filters "Name=name,Values=chamows-*" \
        --query 'sort_by(Images,&CreationDate)[-1].ImageId' --output text)
  [ -n "$AMI" ] && [ "$AMI" != "None" ] || die "no hay ninguna AMI de Chamows en $REGION. Construye una con Packer (ver README)."
fi

MY_IP=$(curl -fsS https://checkip.amazonaws.com | tr -d '[:space:]')
[[ "$MY_IP" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "no pude averiguar tu IP pública"

echo "==> Región $REGION, AMI $AMI, tipo $TYPE, tu IP $MY_IP"

# Security group in the default VPC, open only to your IP.
VPC_ID=$(aws ec2 describe-vpcs --filters Name=isDefault,Values=true --query 'Vpcs[0].VpcId' --output text)
[ "$VPC_ID" != "None" ] || die "la región no tiene VPC por defecto"
SG_ID=$(aws ec2 describe-security-groups --filters "Name=group-name,Values=$SG_NAME" "Name=vpc-id,Values=$VPC_ID" \
        --query 'SecurityGroups[0].GroupId' --output text)
if [ "$SG_ID" = "None" ]; then
  SG_ID=$(aws ec2 create-security-group --group-name "$SG_NAME" --vpc-id "$VPC_ID" \
          --description "Chamows: acceso solo desde la IP de su dueno" --query GroupId --output text)
fi
allow() {  # port
  aws ec2 authorize-security-group-ingress --group-id "$SG_ID" \
    --ip-permissions "IpProtocol=tcp,FromPort=$1,ToPort=$1,IpRanges=[{CidrIp=$MY_IP/32,Description=chamows}]" \
    >/dev/null 2>&1 || true   # already allowed
}
allow 443; allow 80; allow 3389
[ -n "$KEY_NAME" ] && allow 22

# Password: 20 random letters and digits. Only the first 8 count for RDP/VNC.
PASSWORD=$(openssl rand -base64 30 | tr -dc 'A-Za-z0-9' | cut -c1-20)
USER_DATA=$(cat <<EOF
#cloud-config
write_files:
  - path: /etc/chamows/initial-password
    owner: root:root
    permissions: '0600'
    content: '$PASSWORD'
EOF
)

KEY_ARGS=()
[ -n "$KEY_NAME" ] && KEY_ARGS=(--key-name "$KEY_NAME")

INSTANCE_ID=$(aws ec2 run-instances \
  --image-id "$AMI" --instance-type "$TYPE" --count 1 \
  --security-group-ids "$SG_ID" ${KEY_ARGS[@]+"${KEY_ARGS[@]}"} \
  --metadata-options "HttpTokens=required,HttpEndpoint=enabled" \
  --user-data "$USER_DATA" \
  --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=chamows},{Key=Project,Value=chamows}]" \
  --query 'Instances[0].InstanceId' --output text)

echo "==> Instancia $INSTANCE_ID creada, esperando a que arranque…"
aws ec2 wait instance-running --instance-ids "$INSTANCE_ID"
IP=$(aws ec2 describe-instances --instance-ids "$INSTANCE_ID" \
     --query 'Reservations[0].Instances[0].PublicIpAddress' --output text)

cat <<EOF

Chamows está arrancando. El escritorio tarda 1-2 minutos más en estar listo.

  Navegador:  https://$IP   (acepta el aviso del certificado)
  RDP:        $IP:3389       (Escritorio remoto de Windows / Microsoft Remote Desktop)
  Usuario:    chamows
  Contraseña: $PASSWORD

Guarda la contraseña: no se vuelve a mostrar.
Para borrar la máquina y dejar de pagar:  ./scripts/terminate.sh $INSTANCE_ID --region $REGION
EOF

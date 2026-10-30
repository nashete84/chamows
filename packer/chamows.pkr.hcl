# Builds the Chamows AMI on top of the official Ubuntu 24.04 image.
#
#   cd packer
#   packer init .
#   packer build .                         # default region eu-west-1
#   packer build -var region=us-east-1 .

packer {
  required_plugins {
    amazon = {
      version = ">= 1.3.0"
      source  = "github.com/hashicorp/amazon"
    }
  }
}

variable "region" {
  type    = string
  default = "eu-west-1"
}

variable "instance_type" {
  type    = string
  default = "t3.large"
}

variable "disk_size_gb" {
  type    = number
  default = 30
}

variable "version" {
  type    = string
  default = "0.1"
}

locals {
  timestamp = formatdate("YYYYMMDD-hhmm", timestamp())
}

source "amazon-ebs" "chamows" {
  region          = var.region
  instance_type   = var.instance_type
  ami_name        = "chamows-${var.version}-${local.timestamp}"
  ami_description = "Chamows ${var.version}: escritorio estilo Windows sobre Ubuntu 24.04"

  source_ami_filter {
    filters = {
      name                = "ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*"
      root-device-type    = "ebs"
      virtualization-type = "hvm"
    }
    owners      = ["099720109477"] # Canonical
    most_recent = true
  }

  ssh_username = "ubuntu"

  # Builder instance only accepts IMDSv2, like the instances launch.sh creates.
  metadata_options {
    http_endpoint               = "enabled"
    http_tokens                 = "required"
    http_put_response_hop_limit = 1
  }

  launch_block_device_mappings {
    device_name           = "/dev/sda1"
    volume_size           = var.disk_size_gb
    volume_type           = "gp3"
    delete_on_termination = true
  }

  tags = {
    Name    = "chamows-${var.version}"
    Project = "chamows"
    Version = var.version
  }
}

build {
  sources = ["source.amazon-ebs.chamows"]

  provisioner "shell" {
    inline = ["mkdir -p /tmp/chamows"]
  }

  # Upload the repository (rootfs, store, scripts).
  provisioner "file" {
    sources     = ["${path.root}/../rootfs", "${path.root}/../store", "${path.root}/../scripts"]
    destination = "/tmp/chamows/"
  }

  provisioner "shell" {
    execute_command = "sudo -E bash '{{ .Path }}'"
    inline = [
      "cloud-init status --wait || true",
      "bash /tmp/chamows/scripts/install.sh",
      "bash /tmp/chamows/scripts/cleanup-image.sh",
      "rm -rf /tmp/chamows",
    ]
  }
}

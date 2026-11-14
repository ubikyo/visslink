
FROM mcr.microsoft.com/devcontainers/python:1-3.12-bookworm

ENV DEBIAN_FRONTEND=noninteractive \
    VIRTUAL_ENV=/opt/venv \
    PATH=/opt/venv/bin:$PATH \
    PYTHONUNBUFFERED=1

# Supprimer le dépôt Yarn inutile pour VissLink
# (clé GPG manquante dans l'image de base)
RUN rm -f /etc/apt/sources.list.d/yarn.list \
    && grep -rl "dl.yarnpkg.com" /etc/apt/sources.list.d/ 2>/dev/null \
       | xargs -r rm -f \
    ; true

# Outils nécessaires au développement ESPHome / C++
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential \
       cmake \
       ninja-build \
       clang-format \
       git \
       libffi-dev \
       libssl-dev \
       libudev-dev \
       pkg-config \
       usbutils \
    && rm -rf /var/lib/apt/lists/*

# Environnement Python isolé
RUN python -m venv /opt/venv \
    && /opt/venv/bin/python -m pip install --no-cache-dir --upgrade pip \
    && /opt/venv/bin/python -m pip install --no-cache-dir \
       esphome \
       ruff \
       pytest \
    && chmod -R a+rX /opt/venv

USER vscode
WORKDIR /workspaces
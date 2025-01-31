FROM fedora:30

RUN dnf install -y --setopt=install_weak_deps=False --best \
    gcc \
    git \
    make \
    python3-pip \
    sudo && \
python3 -m pip install -U --no-cache-dir \
    pip \
    setuptools \
    wheel && \
pip3 install -U --no-cache-dir \
    pytest \
    requests \
    tox && \
git clone https://github.com/aws/aws-fpga /tmp/aws-fpga --depth 1 && \
source /tmp/aws-fpga/sdk_setup.sh && \
rm -Rf /tmp/aws-fpga && \
dnf erase -y make gcc git && \
rm -Rf /var/cache/dnf/*

FROM $DOCKER_IMAGE_PARENT
MAINTAINER Mike Hommey <mhommey@mozilla.com>

# Ubuntu 24.04 now ships with user 1000 ...
RUN userdel --remove ubuntu || true

### Add worker user and setup its workspace.
RUN mkdir -p /builds && \
    groupadd -g 1000 worker && \
    useradd -u 1000 -g 1000 -d /builds/worker -s /bin/bash -m worker && \
    mkdir -p /builds/worker/workspace && \
    chown -R worker:worker /builds

# Declare default working folder
WORKDIR /builds/worker

VOLUME /builds/worker/checkouts
VOLUME /builds/worker/workspace
VOLUME /builds/worker/tooltool-cache

# Set variable normally configured at login, by the shells parent process, these
# are taken from GNU su manual
ENV HOME=/builds/worker \
    SHELL=/bin/bash \
    USER=worker \
    LOGNAME=worker \
    HOSTNAME=taskcluster-worker

# Set a default command useful for debugging
CMD ["/bin/bash", "--login"]

ARG TASKCLUSTER_ROOT_URL
ARG DOCKER_IMAGE_PACKAGES
RUN /usr/local/sbin/setup_packages.sh $TASKCLUSTER_ROOT_URL $DOCKER_IMAGE_PACKAGES && \
    apt-get update && \
    apt-get dist-upgrade && \
    apt-get install \
      git \
      less \
      make \
      mercurial \
      patch \
      python3 \
      python3-minimal \
      python3-zstandard \
      python3-psutil \
      python3-venv \
      vim-tiny \
      xz-utils \
      zstd \
    && /usr/local/sbin/clean_packages.sh $DOCKER_IMAGE_PACKAGES

# %include testing/mozharness/external_tools/robustcheckout.py
COPY topsrcdir/testing/mozharness/external_tools/robustcheckout.py /usr/local/mercurial/robustcheckout.py

# %include taskcluster/docker/recipes/hgrc
COPY topsrcdir/taskcluster/docker/recipes/hgrc /etc/mercurial/hgrc.d/mozilla.rc

# Add pip configuration, among other things.
# %include taskcluster/docker/recipes/dot-config
COPY topsrcdir/taskcluster/docker/recipes/dot-config /builds/worker/.config

# %include taskcluster/scripts/run-task
COPY topsrcdir/taskcluster/scripts/run-task /builds/worker/bin/run-task

# %include third_party/python/taskcluster_taskgraph/taskgraph/run-task/fetch-content
ADD topsrcdir/third_party/python/taskcluster_taskgraph/taskgraph/run-task/fetch-content /builds/worker/bin/fetch-content

RUN chown -R worker:worker /builds/worker/bin && chmod 755 /builds/worker/bin/*

![Build Status](https://codebuild.eu-west-1.amazonaws.com/badges?uuid=eyJlbmNyeXB0ZWREYXRhIjoiNS9XVFh6SXBsa1hNcTV2amhybTFNZUlBdEJ1N0dCYUVGSUtmYzNaMG1sVTlkUVBBTXVyTXZoRkhuU0M1d0drd1VTU0VZUzlONlJQZlB3OEh2UXREcENNPSIsIml2UGFyYW1ldGVyU3BlYyI6ImdJL29WM2YvdFhnQ1NNRSsiLCJtYXRlcmlhbFNldFNlcmlhbCI6MX0%3D&branch=master)
[![Documentation Status](https://readthedocs.org/projects/drmlib/badge/?version=latest)](https://drmlib.readthedocs.io/en/latest/?badge=latest)
 
# Accelize metering library

Accelize metering library is provided to manage metering sessions between an embedded DRM controller on a FPGA and the Accelize web-service.
See [Documentation](https://drmlib.readthedocs.io/).

## Licenses

Please consult [license](licenses/LICENSE)

## Changelogs

Please consult [CHANGELOG](CHANGELOG)

## Build

### Requirements

* Linux distribution (tested with centos7)
* GCC toolchain with C++11 support (tested with gcc 4.8.5)
* packages to build :
    * cmake (tested with cmake 2.8.12)
    * libcurl-devel (tested with libcurl-devel 7.29.0)
    * jsoncpp-devel (tested with jsoncpp-devel 1.8.5)
    * boost (test with boost-deval 1.53.0)
* packages to build packages :
    * rpm-build (tested with rpm-build 4.11.3)
* packages to build documentation :
    * doxygen (tested with doxygen 1.8.5)
    * graphviz (tested with graphviz 2.30.1)

### Build

Basic build :

```console
$mkdir build
$cd build
$cmake ..
$make
```

Build with documentation :

```console
$mkdir build
$cd build
$cmake -DDOC=ON ..
$make
```

Build packages (RPM and TGZ):

```console
$make package
```

Install from build:

```console
$sudo make install
```

### Usage

Please refer to the doxygen generated documentation that document both C and C++
APIs.

### Create the configuration file

The configuration files is a format used in the library to set various options
of about the design and the environment

```json
{
  "design": {
    "udid": "## Please fill the udid communicated by Accelize for your particular application",
    "boardType": "## Please fill the boardType communicated by Accelize for your particular application"
  },
  "webservice": {
    "oauth2_url": "https://master.metering.accelize.com/o/token/",
    "metering_url": "https://master.metering.accelize.com/auth/metering/genlicense/"
  }
}

```

#### DRM mode selection

By default, the DRM work in "metering" mode, but it is possible to set another
mode by editing the configuration file and adding the ``drm`` section with the
``mode`` key.

*Example with nodelock mode:*

```json
{
  "drm": {
    "mode": "nodelock",
    "license_dir": "path/to/local/license/storage/directory"
  }
}
```

In case of nodelock mode, the ``license_dir`` must be provided to define were to save license locally.

### Create the credential file
Create your credential json file as below

```json
{
  "client_id": "## your client id from Accelstore ##",
  "client_secret": "## your client id from Accelstore ##"
}

```

# Support and enhancement requests
[Contact us](https://www.accelize.com/contact) for any support or enhancement request.

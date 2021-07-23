# coding=utf-8

"""
Copyright (c) 2020 The Orbit Authors. All rights reserved.
Use of this source code is governed by a BSD-style license that can be
found in the LICENSE file.
"""

from conans import ConanFile, CMake, tools
from conans.errors import ConanInvalidConfiguration
import os
import shutil
from io import StringIO
from contrib.python import conan_helpers
from contrib.jupyter import build as build_python
import csv


class OrbitConan(ConanFile):
    name = "OrbitProfiler"
    license = "BSD-2-Clause"
    url = "https://github.com/pierricgimmig/orbitprofiler.git"
    description = "C/C++ Performance Profiler"
    settings = "os", "compiler", "build_type", "arch"
    generators = ["cmake_multi"]
    options = {"system_mesa": [True, False],
               "system_qt": [True, False], "with_gui": [True, False],
               "debian_packaging": [True, False],
               "fPIC": [True, False],
               "crashdump_server": "ANY",
               "with_crash_handling": [True, False],
               "run_tests": [True, False],
               "run_python_tests": [True, False],
               "build_target": "ANY",
               "deploy_opengl_software_renderer": [True, False]}
    default_options = {"system_mesa": True,
                       "system_qt": True, "with_gui": True,
                       "debian_packaging": False,
                       "fPIC": True,
                       "crashdump_server": "",
                       "with_crash_handling": True,
                       "run_tests": True,
                       "run_python_tests": False,
                       "build_target": None,
                       "deploy_opengl_software_renderer": False}
    _orbit_channel = "orbitdeps/stable"
    exports_sources = "CMakeLists.txt", "Orbit*", "bin/*", "cmake/*", "third_party/*", "LICENSE"

    def _version(self):
        if not self.version:
            buf = StringIO()
            self.run("git describe --always --tags", output=buf)
            self.version = buf.getvalue().strip()
            if self.version[0] == 'v':
                self.version = self.version[1:]

        return self.version

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC
        if not self.options.with_gui:
            del self.options.with_crash_handling
            del self.options.crashdump_server
        elif not self.options.with_crash_handling:
            del self.options.crashdump_server

    def build_requirements(self):
        self.build_requires('protoc_installer/3.9.1@bincrafters/stable#0')
        self.build_requires('grpc_codegen/1.27.3@{}'.format(self._orbit_channel))
        self.build_requires('gtest/1.10.0#ef88ba8e54f5ffad7d706062d0731a40', force_host_context=True)

    def requirements(self):
        if self.settings.os != "Windows" and self.options.with_gui and not self.options.system_qt and self.options.system_mesa:
            raise ConanInvalidConfiguration("When disabling system_qt, you also have to "
                                            "disable system mesa.")

        if self.options.deploy_opengl_software_renderer and self.settings.os != "Windows":
            raise ConanInvalidConfiguration("The OpenGL software renderer can only be deployed on Windows")
        if self.options.deploy_opengl_software_renderer and not self.options.with_gui:
            raise ConanInvalidConfiguration("The OpenGL software renderer can only be deployed with GUI enabled.")
        if self.options.deploy_opengl_software_renderer and self.options.system_qt:
            raise ConanInvalidConfiguration("The OpenGL software renderer cannot be deployed when using a system-provided Qt installation.")

        self.requires("abseil/20200923.3")
        self.requires("bzip2/1.0.8")
        self.requires("capstone/4.0.1@{}#0".format(self._orbit_channel))
        self.requires("grpc/1.27.3@{}".format(self._orbit_channel))
        self.requires("c-ares/1.15.0")
        self.requires("llvm-core/12.0.0")
        self.requires("lzma_sdk/19.00@orbitdeps/stable#a7bc173325d7463a0757dee5b08bf7fd")
        self.requires("openssl/1.1.1k")
        self.requires("Outcome/3dae433e@orbitdeps/stable#0")
        self.requires(
            "libprotobuf-mutator/20200506@{}#90ce749ca62b40e9c061d20fae4410e0".format(self._orbit_channel))
        if self.settings.os != "Windows":
            self.requires(
                "libunwindstack/20210709@{}".format(self._orbit_channel))
            self.requires("volk/1.2.170")
            self.requires("vulkan-headers/1.1.114.0")
        self.requires("zlib/1.2.11#9e0c292b60ce77402bd9be60dd68266f")

        if self.options.with_gui and self.options.with_crash_handling:
            self.requires("crashpad/20200624@{}".format(self._orbit_channel))

        if self.options.with_gui:
            self.requires("freetype/2.10.0@bincrafters/stable#0")
            self.requires("freetype-gl/79b03d9@{}".format(self._orbit_channel))
            self.requires("glad/0.1.34")
            self.requires("imgui/1.69@bincrafters/stable#0")
            self.requires("libpng/1.6.37@bincrafters/stable#0")
            self.requires("libssh2/1.9.0#df2b6034da12cc5cb68bd3c5c22601bf")

            if not self.options.system_mesa:
                self.requires("libxi/1.7.10@bincrafters/stable#0")

            if not self.options.system_qt:
                self.requires("qt/5.15.1@{}#e659e981368e4baba1a201b75ddb89b6".format(self._orbit_channel))

        if self.options.deploy_opengl_software_renderer:
            self.requires("llvmpipe/21.0.3@{}#fd5932d6a7fa5fb0af5045eb53c02d18".format(self._orbit_channel))


    def configure(self):
        if self.options.debian_packaging and (self.settings.get_safe("os.platform") != "GGP" or tools.detected_os() != "Linux"):
            raise ConanInvalidConfiguration(
                "Debian packaging is only supported for GGP builds!")

        if self.settings.os != "Windows" and not self.options.fPIC:
            raise ConanInvalidConfiguration(
                "We only support compiling with fPIC enabled!")

        if self.options.with_gui and self.settings.arch == "x86":
            raise ConanInvalidConfiguration(
                "We don't actively support building the UI for 32bit platforms. Please remove this check in conanfile.py if you still want to do so!")

        self.options["gtest"].no_main = True

        if self.settings.os != "Windows":
            self.options["llvm-core"].with_xml2 = False

        if self.options.with_gui:

            if not self.options.system_qt:
                self.options["qt"].qtwebengine = True
                self.options["qt"].qtwebchannel = True
                self.options["qt"].qtwebsockets = True
                self.options["qt"].shared = True
                self.options["qt"].with_sqlite3 = False
                self.options["qt"].with_mysql = False
                self.options["qt"].with_pq = False
                self.options["qt"].with_odbc = False
                self.options["qt"].with_sdl2 = False
                self.options["qt"].with_openal = False

                if self.settings.os == "Windows":
                    self.options["qt"].qttools = True
                    self.options["qt"].with_glib = False
                    self.options["qt"].with_harfbuzz = False
                    self.options["qt"].opengl = "dynamic"


    def build(self):
        cmake = CMake(self)
        cmake.definitions["WITH_GUI"] = "ON" if self.options.with_gui else "OFF"
        if self.options.with_gui:
            if self.options.with_crash_handling:
                cmake.definitions["WITH_CRASH_HANDLING"] = "ON"
                cmake.definitions["CRASHDUMP_SERVER"] = self.options.crashdump_server
            else:
                cmake.definitions["WITH_CRASH_HANDLING"] = "OFF"

        cmake.configure()
        cmake.build(target=str(self.options.build_target) if self.options.build_target else None)
        if self.options.run_tests and not tools.cross_building(self.settings, skip_x64_x86=True) and self.settings.get_safe("os.platform") != "GGP":
            cmake.test(output_on_failure=True)
        if self.options.run_python_tests:
            build_python.main()

    def imports(self):
        dest = os.getenv("CONAN_IMPORT_PATH", "bin")
        self.copy("crashpad_handler*", src="@bindirs",
                  dst=dest, root_package="crashpad")
        if self.options.with_gui:
            for path in self.deps_cpp_info["freetype-gl"].resdirs:
                self.copy("Vera.ttf", src=path, dst="{}/fonts/".format(dest))
                self.copy("Vera.ttf", src=path,
                          dst="{}/fonts/".format("OrbitQt/"))
                self.copy("v3f-t2f-c4f.*", src=path,
                          dst="{}/shaders/".format(dest))
                self.copy("v3f-t2f-c4f.*", src=path,
                          dst="{}/shaders/".format("OrbitQt/"))
        if self.options.deploy_opengl_software_renderer:
            shutil.copyfile(os.path.join(self.deps_cpp_info["llvmpipe"].bin_paths[0], "opengl32.dll"),
                            os.path.join(dest, "opengl32sw.dll"))

        excludes = [
                "*qt*",
                "*licensewizard*",
                "*checklicenses*",
                "*.py",
                "*.pyc",
                "*.cc",
                "*.yml",
                "*.vanilla",
                "*.h",
                "*.pl",
                "*license_template.txt",
                "*.patch",
                "*.QT-LICENSE-AGREEMENT",
                "*.ini",
                "*.js",
                "*license-checker*",
                "*.html",
                "*.json",
                "*README.md"
        ]
        self.copy("LICENSE*", dst="licenses", folder=True, ignore_case=True, excludes=excludes)
        self.copy("LICENCE*", dst="licenses", folder=True, ignore_case=True, excludes=excludes)

        if not self.options.system_qt:
            chromium_licenses = conan_helpers.gather_chromium_licenses(self.deps_cpp_info["qt"].rootpath)
            chromium_licenses.sort(key=lambda license_info: license_info["name"].lower())

            with open(os.path.join(self.install_folder, "NOTICE.Chromium.csv"), "w") as fd:
                writer = csv.DictWriter(fd, fieldnames=["name", "url", "license"], extrasaction='ignore')
                writer.writeheader()

                for license in chromium_licenses:
                    writer.writerow(license)

            with open(os.path.join(self.install_folder, "NOTICE.Chromium"), "w") as fd:
                for license in chromium_licenses:
                    fd.write("================================================================================\n")
                    fd.write("Name: {}\n".format(license["name"]))
                    fd.write("URL: {}\n\n".format(license.get("url", "")))
                    fd.write(open(license["license file"], 'r').read())
                    fd.write("\n\n")

                fd.write("================================================================================\n")



    def package(self):
        if self.options.debian_packaging:
            shutil.rmtree(self.package_folder)
            self.copy("OrbitService", src="bin/",
                      dst="{}-{}/opt/developer/tools/".format(self.name, self._version()))
            self.copy("liborbit.so", src="lib/",
                      dst="{}-{}/opt/developer/tools/".format(self.name, self._version()))
            self.copy("NOTICE",
                      dst="{}-{}/usr/share/doc/{}/".format(self.name, self._version(), self.name))
            self.copy("LICENSE",
                      dst="{}-{}/usr/share/doc/{}/".format(self.name, self._version(), self.name))
            basedir = "{}/{}-{}".format(self.package_folder,
                                        self.name, self._version())
            os.makedirs("{}/DEBIAN".format(basedir), exist_ok=True)
            tools.save("{}/DEBIAN/control".format(basedir), """Package: orbitprofiler
Version: {}
Section: development
Priority: optional
Architecture: amd64
Maintainer: Google, Inc <orbitprofiler-eng@google.com>
Description: Orbit is a C/C++ profiler for Windows, Linux and the Stadia Platform.
Homepage: https://github.com/google/orbit
Installed-Size: `du -ks usr/ | cut -f 1`
""".format(self._version()))

            tools.save("{}/DEBIAN/postinst".format(basedir), """
#!/bin/bash
# Setting the setuid-bit for OrbitService
chmod -v 4775 /opt/developer/tools/OrbitService
""")

            self.run("chmod +x {}/DEBIAN/postinst".format(basedir))
            self.run("chmod g-s {}/DEBIAN".format(basedir))
            self.run("chmod g-s {}/".format(basedir))
            self.run("dpkg-deb -b --root-owner-group {}".format(basedir))
            self.run("dpkg --contents {}.deb".format(basedir))
            shutil.rmtree(basedir)

        self.copy("*", src="bin/fonts", dst="bin/fonts", symlinks=True)
        self.copy("*", src="bin/shaders", dst="bin/shaders", symlinks=True)
        self.copy("*.pdb", src="bin/", dst="bin")
        self.copy("Orbit", src="bin/", dst="bin")
        self.copy("Orbit.exe", src="bin/", dst="bin")
        self.copy("Orbit.debug", src="bin/", dst="bin")
        self.copy("OrbitService", src="bin/", dst="bin")
        self.copy("OrbitService.exe", src="bin/", dst="bin")
        self.copy("OrbitService.debug", src="bin/", dst="bin")
        self.copy("OrbitClientGgp", src="bin/", dst="bin")
        self.copy("OrbitClientGgp.exe", src="bin/", dst="bin")
        self.copy("OrbitClientGgp.debug", src="bin/", dst="bin")
        self.copy("crashpad_handler.exe", src="bin/", dst="bin")
        self.copy("NOTICE")
        self.copy("NOTICE.Chromium")
        self.copy("NOTICE.Chromium.csv")
        self.copy("LICENSE")
        self.copy("liborbit.so", src="lib/", dst="lib")
        self.copy("libOrbitVulkanLayer.so", src="lib/", dst="lib")
        self.copy("VkLayer_Orbit_implicit.json", src="lib/", dst="lib")
        self.copy("LinuxTracingIntegrationTests", src="bin/", dst="bin")
        self.copy("LinuxTracingIntegrationTests.debug", src="bin/", dst="bin")
        self.copy("libLinuxTracingIntegrationTestPuppetSharedObject.so", src="lib/", dst="lib")
        self.copy("libLinuxTracingIntegrationTestPuppetSharedObject.so.debug", src="lib/", dst="lib")
        self.copy("OrbitFakeClient", src="bin/", dst="bin")
        self.copy("OrbitFakeClient.debug", src="bin/", dst="bin")
        self.copy("opengl32sw.dll", src="bin/", dst="bin")

        if not self.options.system_qt:
            orbit_executable = "Orbit.exe" if self.settings.os == "Windows" else "Orbit"
            self.run("windeployqt --pdb --no-angle {}".format(orbit_executable), cwd=os.path.join(self.package_folder, "bin"), run_environment=True)

            # Package Visual C++ and C Runtime
            vcvars = tools.vcvars_dict(self)
            if 'VCToolsRedistDir' in vcvars:
                arch = 'x64' if self.settings.arch == 'x86_64' else 'x86'
                src_path = os.path.join(vcvars['VCToolsRedistDir'], arch, 'Microsoft.VC142.CRT')
                self.copy("*.dll", src=src_path, dst="bin")


    def deploy(self):
        self.copy("*", src="bin", dst="bin")

Name: hotmon
Version: 0.1.0
Release: 1%{?dist}
Summary: Terminal UI for creating and managing wireless hotspots
License: LicenseRef-unspecified
URL: https://github.com/colorlesspilgrimage/hotmon
Source0: %{name}-%{version}.tar.gz

BuildRequires: gcc-c++ cmake pkgconf-pkg-config ncurses-devel yyjson-devel gtest-devel
Requires: polkit iw iproute
Recommends: NetworkManager
Suggests: iwd hostapd nftables dnsmasq

%description
Terminal UI for creating and managing wireless hotspots.

%prep
%autosetup

%build
%cmake -DCMAKE_BUILD_TYPE=Release
%cmake_build

%check
# Run every test. Exclude a test only when a clean container lacks a host tool.
%ctest

%install
%cmake_install

%files
%{_bindir}/hotmon
%{_docdir}/hotmon/README.md

%changelog
* Sun Oct 11 2026 colorlesspilgrimage <colorlesspilgrimage@users.noreply.github.com> - 0.1.0-1
- Initial release.

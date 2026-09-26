# COPR's make_srpm hook: a source RPM of this checkout, versioned like 0.2.0^4.gf97473f

spec := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))../packaging/fedora/aro.spec

srpm:
	dnf -y install git rpm-build
	git config --global --add safe.directory '*'
	set -e; \
	v=$$(git describe --long --tags --abbrev=7 | sed 's/^v//; s/-\([0-9]*\)-g/^\1.g/'); \
	git archive --prefix=aro-$$v/ -o "$(outdir)/aro-$$v.tar.gz" HEAD; \
	sed "s/^Version:.*/Version:        $$v/" "$(spec)" > "$(outdir)/aro.spec"; \
	rpmbuild -bs --define "_sourcedir $(outdir)" --define "_srcrpmdir $(outdir)" "$(outdir)/aro.spec"

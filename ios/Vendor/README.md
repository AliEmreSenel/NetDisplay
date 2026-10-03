# Build-time dependency

No third-party source or binaries are vendored here. `../scripts/prepare-deps.sh` downloads libsodium 1.0.22 from its official release server, verifies the Minisign signature with the upstream public key, and builds it for iPhoneOS arm64. The generated app includes the upstream license. See `../README.md`.

class Frametap < Formula
  desc "Cross-platform screen capture CLI for macOS, Linux, and Windows"
  homepage "https://github.com/krazyjakee/frametap"
  license "MIT"
  version "0.1.3"

  on_macos do
    if Hardware::CPU.arm?
      url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-macos-arm64.zip"
      sha256 "e15e30e65aeb0129676fafcc97003a142d202522683b806766dec7156e3a3af8" # macos-arm64
    else
      url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-macos-x86_64.zip"
      sha256 "b19ee8850e923bfbe966fd828547b23426dc886e243d5bba6077e86e7ae35356" # macos-x86_64
    end
  end

  on_linux do
    url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-linux.zip"
    sha256 "61d02026a8d10d7a73a5583ef3c02f5e0deef4b32595f57772b5a17a81f47ee1" # linux
  end

  def install
    bin.install "frametap"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/frametap --version")
  end
end

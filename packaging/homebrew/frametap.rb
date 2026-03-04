class Frametap < Formula
  desc "Cross-platform screen capture CLI for macOS, Linux, and Windows"
  homepage "https://github.com/krazyjakee/frametap"
  license "MIT"
  version "0.1.3"

  on_macos do
    if Hardware::CPU.arm?
      url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-macos-arm64.zip"
      sha256 "2e13ddbf350f79f3483d6d2d11329ebcf72a41b1538acc713dc67bee565e88e8" # macos-arm64
    else
      url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-macos-x86_64.zip"
      sha256 "0191bdc8e751d0ac1a01feb741dd9fdb3b675f97c85f5eb4a2858d8040a0c135" # macos-x86_64
    end
  end

  on_linux do
    url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-linux.zip"
    sha256 "1abc16463f17d00e01c1957e7d043849f5618ed872d13b16593512272fa2ad09" # linux
  end

  def install
    bin.install "frametap"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/frametap --version")
  end
end

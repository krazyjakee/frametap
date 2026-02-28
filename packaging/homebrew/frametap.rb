class Frametap < Formula
  desc "Cross-platform screen capture CLI for macOS, Linux, and Windows"
  homepage "https://github.com/krazyjakee/frametap"
  license "MIT"
  version "0.1.2"

  on_macos do
    url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-macos.zip"
    sha256 "9b901649d647c758e23d37502fd840f5783da874464f6e63cdc46ca9f86a6d31"
  end

  on_linux do
    url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-linux.zip"
    sha256 "3337570b2e4ac6e81c1d4ed963f30995125cd446eabc1662a07b7e9a9342205a"
  end

  def install
    bin.install "frametap"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/frametap --version")
  end
end

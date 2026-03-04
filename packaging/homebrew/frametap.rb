class Frametap < Formula
  desc "Cross-platform screen capture CLI for macOS, Linux, and Windows"
  homepage "https://github.com/krazyjakee/frametap"
  license "MIT"
  version "0.1.3"

  on_macos do
    if Hardware::CPU.arm?
      url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-macos-arm64.zip"
      sha256 "b997f09857bea7a9fccb81583a84122df586b0a56e178e1f8d8bb6b5e00c4d79" # macos-arm64
    else
      url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-macos-x86_64.zip"
      sha256 "b9d9a77b907689acef56fefe2bd44b9114eba968f2cdb74746830b3a8fa736bb" # macos-x86_64
    end
  end

  on_linux do
    url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-linux.zip"
    sha256 "40f7e1e2424bb68e210d7ccd063c8f6490d94845436225eb6daa62f28bf9fc72" # linux
  end

  def install
    bin.install "frametap"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/frametap --version")
  end
end

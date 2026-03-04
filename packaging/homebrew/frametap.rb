class Frametap < Formula
  desc "Cross-platform screen capture CLI for macOS, Linux, and Windows"
  homepage "https://github.com/krazyjakee/frametap"
  license "MIT"
  version "0.1.3"

  on_macos do
    if Hardware::CPU.arm?
      url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-macos-arm64.zip"
      sha256 "216212b335eacd595826f4803229452647a535688222246a1d2cd7eba8199bf1" # macos-arm64
    else
      url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-macos-x86_64.zip"
      sha256 "ed0c23f652583c4db1fdd4463d493af20fceebe980d3a49b41dc46fc968609cf" # macos-x86_64
    end
  end

  on_linux do
    url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-linux.zip"
    sha256 "ff507e52f1d6b94c60d94392809802c0f45a04400f100c6f0f81214e929cf602" # linux
  end

  def install
    bin.install "frametap"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/frametap --version")
  end
end

class Ranvier < Formula
  desc "Layer 7+ LLM traffic controller with prefix-aware routing"
  homepage "https://github.com/ranvier-systems/ranvier-core"
  license "Apache-2.0"

  # TODO: Update with actual release URL and SHA
  # url "https://github.com/ranvier-systems/ranvier-core/releases/download/v#{version}/ranvier-#{version}-darwin-arm64.tar.gz"
  # sha256 "TODO"

  def install
    bin.install "ranvier_server" => "ranvier"
    share.install "assets/gpt2.json" => "ranvier/gpt2.json"
    pkgshare.install "ranvier-local.yaml.example"
  end

  def caveats
    <<~EOS
      To start Ranvier in local mode:
        ranvier --local

      This auto-discovers Ollama, vLLM, LM Studio, and other
      local LLM servers. No configuration needed.

      For cloud/cluster deployment, see:
        #{pkgshare}/ranvier-local.yaml.example
    EOS
  end

  service do
    run [opt_bin/"ranvier", "--local"]
    keep_alive true
    log_path var/"log/ranvier.log"
  end

  test do
    # Quick validation that binary runs
    system "#{bin}/ranvier", "--dry-run", "--local"
  end
end

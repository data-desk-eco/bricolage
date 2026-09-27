// menu bar view over a bricolage db: reads with libsqlite3, writes by
// inserting keys through homebrew's sqlite3 with the extension loaded
import SwiftUI
import SQLite3

struct Row: Identifiable { let id: String, key, state: String, age, pages: Int }
struct Job: Identifiable { let id: String; var rows: [Row], total: Int }

let Q = """
with l as (select job, key, attempt, kind, ts from bric_log
  where kind in ('open', 'close', 'error')),
t as (select job, key, max(attempt) as a from l group by 1, 2)
select l.job, l.key, case when sum(kind = 'close') then 'done'
  when sum(kind = 'error') then 'failed' else 'running' end,
  cast((julianday('now') - julianday(min(ts))) * 86400 as int),
  (select count(*) from bric_log as r where r.job = l.job
    and r.key = l.key and r.kind = 'receipt')
from l join t on l.job = t.job and l.key = t.key and attempt = a
group by 1, 2 order by min(ts) desc
"""

func rows(_ db: OpaquePointer?, _ sql: String) -> [[String]] {
  var s: OpaquePointer?, out: [[String]] = []
  guard sqlite3_prepare_v2(db, sql, -1, &s, nil) == SQLITE_OK else { return [] }
  while sqlite3_step(s) == SQLITE_ROW {
    out.append((0..<sqlite3_column_count(s)).map {
      sqlite3_column_text(s, $0).map { String(cString: $0) } ?? "" })
  }
  sqlite3_finalize(s); return out
}

func q(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'",
  with: "''") + "'" }

final class Model: ObservableObject {
  @AppStorage("db") var path = ""
  @Published var jobs: [Job] = []
  var timer: Timer?
  init() {
    timer = .scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self]
      _ in self?.load() }
    load()
  }
  func load() {
    var db: OpaquePointer?
    guard !path.isEmpty, sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY,
      nil) == SQLITE_OK else { return }
    defer { sqlite3_close(db) }
    let log = Dictionary(grouping: rows(db, Q), by: { $0[0] })
    // bric_log.job holds the brief
    jobs = rows(db, "select source, brief from bric_job").map { j in
      let n = rows(db, "select count(*) from \(j[0])").first?[0] ?? "0"
      return Job(id: j[0], rows: (log[j[1]] ?? []).map {
        Row(id: $0[1], key: $0[1], state: $0[2], age: Int($0[3]) ?? 0,
            pages: Int($0[4]) ?? 0) }, total: Int(n) ?? 0)
    }
  }
  // zsh -i so workers inherit the BRIC_* keys from ~/.zshrc
  func run(_ sql: String) {
    let u = URL(fileURLWithPath: path), p = Process()
    p.executableURL = URL(fileURLWithPath: "/bin/zsh")
    p.currentDirectoryURL = u.deletingLastPathComponent()
    p.environment = ["DB": u.path, "SQL": sql,
                     "HOME": NSHomeDirectory()]
    p.arguments = ["-ic", "${SQLITE:-/opt/homebrew/opt/sqlite/bin/sqlite3}"
      + " \"$DB\" -cmd '.load ./ext/bric' \"$SQL\""]
    try? p.run()
    DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self.load() }
  }
  func add(_ job: String, _ text: String) {
    let ks = text.split(separator: "\n").map {
      q($0.trimmingCharacters(in: .whitespaces)) }.filter { $0 != "''" }
    if !ks.isEmpty { run("insert or replace into \(job) (key) values ("
      + ks.joined(separator: "), (") + ")") }
  }
  func retry(_ job: String, _ key: String) {
    run("insert or replace into \(job) select * from \(job) where key = "
      + q(key))
  }
  func open() {
    let p = NSOpenPanel()
    NSApp.activate(ignoringOtherApps: true)
    if p.runModal() == .OK, let u = p.url { path = u.path; load() }
  }
}

func ago(_ s: Int) -> String { s < 60 ? "\(s)s" : s < 3600 ? "\(s / 60)m"
  : s < 86400 ? "\(s / 3600)h" : "\(s / 86400)d" }

struct Dot: View {
  let state: String
  @State var on = false
  var body: some View {
    Group {
      switch state {
      case "done": Image(systemName: "circle.fill")
      case "failed": Image(systemName: "xmark")
      default: Image(systemName: "circle.dotted").opacity(on ? 0.3 : 1)
        .onAppear { withAnimation(.easeInOut(duration: 0.8)
          .repeatForever()) { on = true } }
      }
    }.font(.system(size: 8, weight: .bold)).frame(width: 12)
  }
}

struct JobView: View {
  @ObservedObject var m: Model
  let job: Job
  @State var open = true
  @State var text = ""
  var body: some View {
    let done = job.rows.filter { $0.state == "done" }.count
    DisclosureGroup(isExpanded: $open) {
      ForEach(job.rows.prefix(12)) { r in
        HStack(spacing: 6) {
          Dot(state: r.state)
          Text(r.key).lineLimit(1).truncationMode(.middle)
          Spacer()
          Text("\(r.pages)p").foregroundStyle(.secondary)
          Text(ago(r.age)).foregroundStyle(.secondary)
            .frame(width: 30, alignment: .trailing)
          if r.state == "failed" {
            Button { m.retry(job.id, r.key) } label: {
              Image(systemName: "arrow.clockwise") }.buttonStyle(.plain)
          }
        }.font(.system(size: 11, design: .monospaced))
      }
      TextField("+ key", text: $text).textFieldStyle(.plain)
        .font(.system(size: 11, design: .monospaced))
        .onSubmit { m.add(job.id, text); text = "" }
    } label: {
      HStack {
        Text(job.id).font(.system(.body, design: .monospaced).bold())
        ProgressView(value: Double(done), total: Double(max(job.total, 1)))
        Text("\(done)/\(job.total)").monospacedDigit()
          .foregroundStyle(.secondary)
      }
    }
  }
}

@main struct Bric: App {
  @StateObject var m = Model()
  init() { NSApplication.shared.setActivationPolicy(.accessory) }
  var body: some Scene {
    let n = m.jobs.flatMap(\.rows).filter { $0.state == "running" }.count
    MenuBarExtra {
      VStack(alignment: .leading, spacing: 8) {
        Text(m.path.isEmpty ? "no db" : (m.path as NSString).lastPathComponent)
          .font(.headline)
        ScrollView {
          VStack(alignment: .leading) {
            ForEach(m.jobs) { JobView(m: m, job: $0) }
          }
        }.frame(maxHeight: 420)
        Divider()
        HStack {
          Button("open db…") { m.open() }
          Spacer()
          Button("quit") { NSApp.terminate(nil) }
        }.buttonStyle(.plain).foregroundStyle(.secondary)
      }.padding(12).frame(width: 340)
    } label: {
      Image(systemName: n > 0 ? "circle.dotted.circle" : "circle.circle")
      if n > 0 { Text("\(n)") }
    }.menuBarExtraStyle(.window)
  }
}

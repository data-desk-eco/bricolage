// menu bar view over a bricolage db: reads with libsqlite3, writes by
// inserting keys through homebrew's sqlite3 with the extension loaded
import SwiftUI
import SQLite3

struct Row: Identifiable { let id: String, key, state: String, age, calls: Int }
struct Job: Identifiable {
  let id, target: String; var rows: [Row], done, total: Int
}

// a key's latest attempt, its job named by target even in logs from
// before the engine stopped naming jobs by their brief
let Q = """
select coalesce(j.target, a.job), a.key, case a.kind when 'close' then 'done'
  when 'error' then 'failed' else 'running' end, a.age, a.calls
from bric_attempt as a left join bric_job as j on a.job = j.brief
order by a.ts desc
"""

// with head, the first row is the column names
func rows(_ db: OpaquePointer?, _ sql: String, head: Bool = false)
  -> [[String]] {
  var s: OpaquePointer?, out: [[String]] = []
  guard sqlite3_prepare_v2(db, sql, -1, &s, nil) == SQLITE_OK else { return [] }
  if head { out.append((0..<sqlite3_column_count(s)).map {
    String(cString: sqlite3_column_name(s, $0)) }) }
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
  // a read-only open fails on a WAL db without a writable shm, so open
  // read-write and forbid writes on the connection instead
  func read(_ sql: String, head: Bool = false) -> [[String]] {
    var db: OpaquePointer?
    guard !path.isEmpty, sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE,
      nil) == SQLITE_OK else { return [] }
    defer { sqlite3_close(db) }
    sqlite3_exec(db, "pragma query_only = 1", nil, nil, nil)
    return rows(db, sql, head: head)
  }
  func table(_ t: String) -> [[String]] {
    read("select * from \"\(t)\"", head: true)
  }
  // one key's log in order, replies as their thinking and text, calls as
  // their command
  func log(_ t: String, _ key: String) -> [[String]] { read("""
    select seq, attempt, turn, kind, case kind
      when 'open' then concat_ws(char(10) || char(10), detail ->> 'system',
        detail ->> 'message', 'model ' || nullif(detail ->> 'model', ''))
      when 'reply' then (select group_concat(coalesce(value ->> 'thinking',
        value ->> 'text'), char(10) || char(10)) from json_each(detail)
        where value ->> 'type' in ('thinking', 'text'))
      when 'call' then coalesce(detail ->> 'command', detail)
      else coalesce(text, detail) end
    from bric_log where key = \(q(key)) and job in (\(q(t)),
      (select brief from bric_job where target = \(q(t)))) order by seq
    """) }
  // a key is done when its target has a row for it, as the engine counts
  func load() {
    var seen = Set<[String]>()
    let log = Dictionary(grouping: read(Q).filter {
      seen.insert([$0[0], $0[1]]).inserted }, by: { $0[0] })
    jobs = read("select source, target from bric_job").map { j in
      let (s, t) = ("\"\(j[0])\"", "\"\(j[1])\""), n = read("""
        select count(*) filter (where key in (select key from \(t))),
          count(*) from \(s)
        """).first ?? ["0", "0"]
      return Job(id: j[0], target: j[1], rows: (log[j[1]] ?? []).map {
        Row(id: $0[1], key: $0[1], state: $0[2], age: Int($0[3]) ?? 0,
            calls: Int($0[4]) ?? 0) }, done: Int(n[0]) ?? 0,
        total: Int(n[1]) ?? 0)
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
  // every key its target has no row for, inserted again as the readme does
  func pending(_ j: Job) {
    run("insert or replace into \"\(j.id)\" select * from \"\(j.id)\" "
      + "where key not in (select key from \"\(j.target)\")")
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
  var body: some View {
    Group {
      switch state {
      case "done": Image(systemName: "circle.fill")
      case "failed": Image(systemName: "xmark")
      default: Image(systemName: "circle.dotted").symbolEffect(.pulse)
      }
    }.font(.system(size: 8, weight: .bold)).frame(width: 12)
  }
}

struct JobView: View {
  @ObservedObject var m: Model
  @Environment(\.openWindow) var win
  let job: Job
  @State var open = true
  @State var text = ""
  var body: some View {
    DisclosureGroup(isExpanded: $open) {
      ForEach(job.rows.prefix(12)) { r in
        HStack(spacing: 6) {
          Button { NSApp.activate(ignoringOtherApps: true)
            win(id: "log", value: Open(t: job.target, key: r.key)) } label: {
          HStack(spacing: 6) { Dot(state: r.state)
          Text(r.key).lineLimit(1).truncationMode(.middle)
          Spacer()
          Text("\(r.calls)c").foregroundStyle(.secondary)
          Text(ago(r.age)).foregroundStyle(.secondary)
            .frame(width: 30, alignment: .trailing)
          }.contentShape(Rectangle()) }.buttonStyle(.plain)
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
        Button { NSApp.activate(ignoringOtherApps: true)
          win(value: Open(t: job.target, key: nil)) } label: {
          Text(job.id).font(.system(.body, design: .monospaced).bold())
        }.buttonStyle(.plain).help("open \(job.target)")
        ProgressView(value: Double(job.done), total: Double(max(job.total, 1)))
        Text("\(job.done)/\(job.total)").monospacedDigit()
          .foregroundStyle(.secondary)
        if job.done < job.total {
          Button { m.pending(job) } label: { Image(systemName: "play.fill") }
            .buttonStyle(.plain).help("run the keys \(job.target) lacks")
        }
      }
    }
  }
}

// a result table in a native Table, re-read every two seconds while open
struct Rec: Identifiable { let id: Int, v: [String] }
// a window's table and, for a transcript window, the key whose session it shows
struct Open: Codable, Hashable { var t: String, key: String? }
struct DataView: View {
  let m: Model, t: String
  @Environment(\.openWindow) var win
  @State var d: [[String]] = []
  @State var sel: Int?
  var body: some View {
    let h = d.first ?? [], k = h.firstIndex(of: "key") ?? 0,
      r = d.dropFirst().enumerated().map { Rec(id: $0, v: $1) }
    Table(r, selection: $sel) {
      TableColumnForEach(h.indices, id: \.self) { i in
        TableColumn(h[i]) { Text($0.v[i]).help($0.v[i]) }
      }
    }.font(.system(size: 11, design: .monospaced))
      .contextMenu(forSelectionType: Int.self) { _ in } primaryAction: {
        for s in $0 where r.indices.contains(s) {
          win(id: "log", value: Open(t: t, key: r[s].v[k])) } }
      .navigationTitle("\(t) · \(r.count) rows")
      .frame(minWidth: 500, minHeight: 300)
      .task { while !Task.isCancelled {
        d = m.table(t); try? await Task.sleep(for: .seconds(2)) } }
  }
}

// a row's session: every log entry of its key, latest attempt last
struct Log: View {
  let m: Model, t: String, key: String
  @State var l: [[String]] = []
  @State var full = Set<String>()
  var body: some View {
    List(l, id: \.[0]) { e in
      let cut = e[3] == "receipt" ? 8 : e[3] == "open" ? 1 << 20 : 40,
        long = e[4].split(separator: "\n", omittingEmptySubsequences: false)
          .count > cut || e[4].count > cut * 120
      VStack(alignment: .leading, spacing: 4) {
        HStack {
          Text(e[3]).bold().foregroundStyle(e[3] == "error" ? .red
            : e[3] == "call" ? .blue : .primary)
          Spacer()
          Text("attempt \(e[1]) · turn \(e[2]) · #\(e[0])")
            .foregroundStyle(.secondary)
          if long { Image(systemName: full.contains(e[0])
            ? "chevron.up" : "chevron.down").foregroundStyle(.secondary) }
        }.font(.system(size: 10, design: .monospaced))
          .contentShape(Rectangle()).onTapGesture {
            if full.remove(e[0]) == nil { full.insert(e[0]) } }
        Text(e[4]).font(.system(size: 11,
          design: ["reply", "open"].contains(e[3]) ? .default : .monospaced))
          .lineLimit(full.contains(e[0]) || !long ? nil : cut).textSelection(.enabled)
      }.padding(.vertical, 2)
    }.navigationTitle(key).frame(minWidth: 360, minHeight: 300)
      .task { while !Task.isCancelled {
        l = m.log(t, key); try? await Task.sleep(for: .seconds(2)) } }
  }
}

// a menu bar label ignores its font, so the b is drawn into a template
let logo = {
  let s = NSAttributedString(string: "b", attributes: [.font:
    NSFont.monospacedSystemFont(ofSize: 16, weight: .semibold)])
  let i = NSImage(size: s.size(), flipped: false) { s.draw(in: $0); return true }
  i.isTemplate = true; return i
}()

@main struct Bric: App {
  @StateObject var m = Model()
  init() { NSApplication.shared.setActivationPolicy(.accessory) }
  var body: some Scene {
    let n = m.jobs.flatMap(\.rows).filter { $0.state == "running" }.count
    MenuBarExtra {
      VStack(alignment: .leading, spacing: 8) {
        Text(m.path.isEmpty ? "no db" : (m.path as NSString).lastPathComponent)
          .font(.headline)
        ForEach(m.jobs) { JobView(m: m, job: $0) }
        Divider()
        HStack {
          Button("open db…") { m.open() }
          Spacer()
          Button("quit") { NSApp.terminate(nil) }
        }.buttonStyle(.plain).foregroundStyle(.secondary)
      }.padding(12).frame(width: 340)
    } label: {
      Image(nsImage: logo)
      if n > 0 { Text("\(n)") }
    }.menuBarExtraStyle(.window)
    WindowGroup(for: Open.self) { $o in
      if let o { DataView(m: m, t: o.t) } }.defaultLaunchBehavior(.suppressed)
    WindowGroup(id: "log", for: Open.self) { $o in
      if let o, let k = o.key { Log(m: m, t: o.t, key: k) } }
      .defaultSize(width: 420, height: 760).defaultLaunchBehavior(.suppressed)
  }
}

use std::collections::{BTreeMap, BTreeSet, VecDeque};

const GRAPH_CAPACITY: usize = 40;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ClientSnapshot {
    pub mac: String,
    pub ip: Option<String>,
    pub rx_bytes: u64,
    pub tx_bytes: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Series {
    samples: VecDeque<u64>,
    last: Option<u64>,
    capacity: usize,
}

impl Series {
    pub fn new(capacity: usize) -> Self {
        Self {
            samples: VecDeque::new(),
            last: None,
            capacity,
        }
    }

    pub fn observe(&mut self, total: u64) {
        let delta = match self.last {
            Some(previous) if total >= previous => total - previous,
            Some(_) => 0,
            None => 0,
        };
        self.last = Some(total);
        if self.capacity > 0 && self.samples.len() == self.capacity {
            self.samples.pop_front();
        }
        self.samples.push_back(delta);
    }

    pub fn samples(&self) -> Vec<u64> {
        self.samples.iter().copied().collect()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ClientTraffic {
    pub mac: String,
    pub ip: Option<String>,
    pub rx_bytes: u64,
    pub tx_bytes: u64,
    pub graph: Series,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MonitorState {
    clients: BTreeMap<String, ClientTraffic>,
    total: Series,
}

impl MonitorState {
    pub fn new() -> Self {
        Self {
            clients: BTreeMap::new(),
            total: Series::new(GRAPH_CAPACITY),
        }
    }

    pub fn update(&mut self, clients: &[ClientSnapshot]) {
        let mut seen = BTreeSet::new();
        let mut total_now = 0_u64;
        for client in clients {
            let mac = client.mac.to_ascii_lowercase();
            let bytes = client.rx_bytes.saturating_add(client.tx_bytes);
            total_now = total_now.saturating_add(bytes);
            seen.insert(mac.clone());
            let entry = self
                .clients
                .entry(mac.clone())
                .or_insert_with(|| ClientTraffic {
                    mac: mac.clone(),
                    ip: None,
                    rx_bytes: 0,
                    tx_bytes: 0,
                    graph: Series::new(GRAPH_CAPACITY),
                });
            entry.ip = client.ip.clone();
            entry.rx_bytes = client.rx_bytes;
            entry.tx_bytes = client.tx_bytes;
            entry.graph.observe(bytes);
        }
        self.clients.retain(|mac, _| seen.contains(mac));
        self.total.observe(total_now);
    }

    pub fn clients(&self) -> Vec<ClientTraffic> {
        self.clients.values().cloned().collect()
    }

    pub fn total_samples(&self) -> Vec<u64> {
        self.total.samples()
    }

    pub fn clear(&mut self) {
        self.clients.clear();
        self.total = Series::new(GRAPH_CAPACITY);
    }
}

impl Default for MonitorState {
    fn default() -> Self {
        Self::new()
    }
}

pub fn parse_station_dump(text: &str) -> Vec<ClientSnapshot> {
    let mut clients = Vec::new();
    let mut current: Option<ClientSnapshot> = None;
    for line in text.lines() {
        let trimmed = line.trim();
        if let Some(rest) = trimmed.strip_prefix("Station ") {
            if let Some(done) = current.take() {
                clients.push(done);
            }
            let mac = rest
                .split_whitespace()
                .next()
                .unwrap_or("")
                .to_ascii_lowercase();
            current = Some(ClientSnapshot {
                mac,
                ip: None,
                rx_bytes: 0,
                tx_bytes: 0,
            });
            continue;
        }
        let Some(client) = current.as_mut() else {
            continue;
        };
        if let Some(value) = value_after(trimmed, "rx bytes:") {
            client.rx_bytes = parse_counter(value);
        } else if let Some(value) = value_after(trimmed, "tx bytes:") {
            client.tx_bytes = parse_counter(value);
        }
    }
    if let Some(done) = current {
        clients.push(done);
    }
    clients
}

pub fn parse_neigh(text: &str) -> Vec<(String, String)> {
    let mut pairs = Vec::new();
    for line in text.lines() {
        let parts: Vec<&str> = line.split_whitespace().collect();
        let Some(position) = parts.iter().position(|part| *part == "lladdr") else {
            continue;
        };
        if parts.is_empty() || position + 1 >= parts.len() {
            continue;
        }
        pairs.push((
            parts[0].to_string(),
            parts[position + 1].to_ascii_lowercase(),
        ));
    }
    pairs
}

pub fn clients_from_text(dump: &str, neigh: &str) -> Vec<ClientSnapshot> {
    let mut clients = parse_station_dump(dump);
    let pairs = parse_neigh(neigh);
    for client in &mut clients {
        client.ip = pairs
            .iter()
            .find(|(_, mac)| mac == &client.mac)
            .map(|(ip, _)| ip.clone());
    }
    clients
}

fn value_after<'a>(line: &'a str, label: &str) -> Option<&'a str> {
    line.strip_prefix(label).map(str::trim)
}

fn parse_counter(text: &str) -> u64 {
    text.split_whitespace()
        .next()
        .unwrap_or("0")
        .parse()
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn station_dump_reads_byte_counters() {
        let text = "\
Station AA:BB:CC:DD:EE:FF (on wlan0)
\tinactive time:\t10 ms
\trx bytes:\t1000
\trx packets:\t8
\ttx bytes:\t2500
Station 11:22:33:44:55:66 (on wlan0)
\trx bytes:\t40
\ttx bytes:\t50
";
        let clients = parse_station_dump(text);
        assert_eq!(clients.len(), 2);
        assert_eq!(clients[0].mac, "aa:bb:cc:dd:ee:ff");
        assert_eq!(clients[0].rx_bytes, 1000);
        assert_eq!(clients[0].tx_bytes, 2500);
        assert_eq!(clients[1].rx_bytes, 40);
        assert_eq!(clients[1].tx_bytes, 50);
    }

    #[test]
    fn neigh_adds_the_client_address() {
        let dump = "Station aa:bb:cc:dd:ee:ff (on wlan0)\n\trx bytes:\t1\n\ttx bytes:\t2\n";
        let neigh = "192.168.42.20 lladdr aa:bb:cc:dd:ee:ff REACHABLE\n";
        let clients = clients_from_text(dump, neigh);
        assert_eq!(clients[0].ip.as_deref(), Some("192.168.42.20"));
    }

    #[test]
    fn graph_changes_when_the_byte_counter_changes() {
        let mut series = Series::new(8);
        series.observe(100);
        series.observe(140);
        series.observe(140);
        assert_eq!(series.samples(), vec![0, 40, 0]);
        series.observe(180);
        assert_eq!(*series.samples().last().unwrap(), 40);
    }

    #[test]
    fn total_graph_adds_each_client() {
        let mut monitor = MonitorState::new();
        monitor.update(&[
            ClientSnapshot {
                mac: "aa:bb:cc:dd:ee:ff".to_string(),
                ip: None,
                rx_bytes: 0,
                tx_bytes: 0,
            },
            ClientSnapshot {
                mac: "11:22:33:44:55:66".to_string(),
                ip: None,
                rx_bytes: 0,
                tx_bytes: 0,
            },
        ]);
        monitor.update(&[
            ClientSnapshot {
                mac: "aa:bb:cc:dd:ee:ff".to_string(),
                ip: None,
                rx_bytes: 10,
                tx_bytes: 0,
            },
            ClientSnapshot {
                mac: "11:22:33:44:55:66".to_string(),
                ip: None,
                rx_bytes: 0,
                tx_bytes: 5,
            },
        ]);
        assert_eq!(monitor.total_samples(), vec![0, 15]);
        let clients = monitor.clients();
        assert_eq!(clients[0].graph.samples(), vec![0, 5]);
        assert_eq!(clients[1].graph.samples(), vec![0, 10]);
    }
}

use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};

use crate::capture::FrameSource;
use crate::iface;

pub struct LocalCapture {
    fd: OwnedFd,
}

fn invalid_capture_name() -> std::io::Error {
    std::io::Error::new(
        std::io::ErrorKind::InvalidInput,
        "The capture interface name is not valid.",
    )
}

impl LocalCapture {
    pub fn open(interface: &str) -> std::io::Result<Self> {
        if !iface::valid_name(interface) {
            return Err(invalid_capture_name());
        }
        let fd = unsafe {
            libc::socket(
                libc::AF_PACKET,
                libc::SOCK_RAW,
                (libc::ETH_P_ALL as u16).to_be() as i32,
            )
        };
        if fd < 0 {
            return Err(std::io::Error::last_os_error());
        }
        let fd = unsafe { OwnedFd::from_raw_fd(fd) };
        let name = std::ffi::CString::new(interface).map_err(|_| invalid_capture_name())?;
        let index = unsafe { libc::if_nametoindex(name.as_ptr()) };
        if index == 0 {
            return Err(std::io::Error::last_os_error());
        }
        let mut address: libc::sockaddr_ll = unsafe { std::mem::zeroed() };
        address.sll_family = libc::AF_PACKET as u16;
        address.sll_protocol = (libc::ETH_P_ALL as u16).to_be();
        address.sll_ifindex = index as i32;
        let rc = unsafe {
            libc::bind(
                fd.as_raw_fd(),
                &address as *const libc::sockaddr_ll as *const libc::sockaddr,
                std::mem::size_of::<libc::sockaddr_ll>() as libc::socklen_t,
            )
        };
        if rc != 0 {
            return Err(std::io::Error::last_os_error());
        }
        Ok(Self { fd })
    }
}

impl FrameSource for LocalCapture {
    fn try_recv(&mut self) -> std::io::Result<Option<Vec<u8>>> {
        let mut buffer = vec![0_u8; 65535];
        let size = unsafe {
            libc::recv(
                self.fd.as_raw_fd(),
                buffer.as_mut_ptr() as *mut libc::c_void,
                buffer.len(),
                libc::MSG_DONTWAIT,
            )
        };
        if size < 0 {
            let err = std::io::Error::last_os_error();
            if err.kind() == std::io::ErrorKind::WouldBlock
                || err.raw_os_error() == Some(libc::EAGAIN)
                || err.raw_os_error() == Some(libc::EWOULDBLOCK)
            {
                return Ok(None);
            }
            return Err(err);
        }
        buffer.truncate(size as usize);
        Ok(Some(buffer))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn invalid_interface_does_not_open_a_socket() {
        match LocalCapture::open("") {
            Err(error) => assert_eq!(error.kind(), std::io::ErrorKind::InvalidInput),
            Ok(_) => panic!("an empty name must not open a socket"),
        }
        match LocalCapture::open("not a name") {
            Err(error) => assert_eq!(error.kind(), std::io::ErrorKind::InvalidInput),
            Ok(_) => panic!("a bad name must not open a socket"),
        }
    }
}
